"""NumPy 批量接口：合规输入数组直接传入 C ABI，输出使用 NumPy 存储。"""
from __future__ import annotations
import ctypes as c
from dataclasses import dataclass
import operator
import numpy as np
from vagrad_kernel import Kernel, KernelError


@dataclass
class ArrayResult:
    values: np.ndarray
    jacobian: np.ndarray
    valid: np.ndarray
    status: np.ndarray
    tokens: np.ndarray
    output_names: tuple[str, ...]
    seed_names: tuple[str, ...]
    copied_inputs: dict[str, bool]

    @property
    def ok(self):
        return self.status == 0

    def require_all(self):
        failed = np.flatnonzero(~self.ok)
        if failed.size:
            row = int(failed[0])
            raise KernelError(int(self.status[row]), int(self.tokens[row]))
        return self

    def observation(self, name):
        return self.values[:, self.output_names.index(name)]

    def derivative(self, output, seed):
        return self.jacobian[:, self.output_names.index(output), self.seed_names.index(seed)]


class NumpyKernel(Kernel):
    def evaluate_arrays(self, *, parameters=None, voltages=None, temperatures=None,
                        count=None, copy=False, out=None, step_limit=100000):
        """数组以样本为第一维。copy=False 要求 native float64、C 连续且对齐。"""
        arrays = {}
        copied = {}
        for name, value in (("parameters", parameters), ("voltages", voltages), ("temperatures", temperatures)):
            if value is None:
                arrays[name], copied[name] = None, False
                continue
            if not copy and not isinstance(value, np.ndarray):
                raise TypeError(f"{name} must be a NumPy array when copy=False")
            original = value
            if copy:
                array = np.require(np.asarray(value), dtype=np.float64, requirements=["C", "A"])
                copied[name] = not isinstance(original, np.ndarray) or (
                    array is not original and not np.shares_memory(original, array))
            else:
                array = value
                if array.dtype != np.dtype(np.float64) or not array.flags.c_contiguous or not array.flags.aligned:
                    raise ValueError(f"{name} requires native float64, aligned C-contiguous storage; use copy=True")
                copied[name] = False
            arrays[name] = array
        inferred = []
        for name, columns in (("parameters", len(self.parameters)), ("voltages", len(self.nodes))):
            array = arrays[name]
            if array is not None:
                if array.ndim != 2 or array.shape[1] != columns:
                    raise ValueError(f"{name} shape must be (samples, {columns})")
                inferred.append(array.shape[0])
        temperature_array = arrays["temperatures"]
        if temperature_array is not None:
            if temperature_array.ndim != 1:
                raise ValueError("temperatures shape must be (samples,)")
            inferred.append(temperature_array.shape[0])
        if count is None:
            if not inferred:
                raise ValueError("count is required when all inputs are None")
            count = inferred[0]
        if isinstance(count, (bool, np.bool_)):
            raise ValueError("count must be a nonnegative integer")
        count = operator.index(count)
        self._limit(count)
        if any(n != count for n in inferred):
            raise ValueError("all input arrays must have the same sample count")
        for k, name in enumerate(self.parameters):
            if arrays["parameters"] is not None and not self.overridable[k]:
                if np.any(arrays["parameters"][:, k] != self.defaults[k]):
                    raise ValueError(f"localparam cannot be overridden: {name}")

        n, lanes = len(self.outputs), len(self.seeds)
        specs = (
            ("values", (count, n), np.float64),
            ("jacobian", (count, n, lanes), np.float64),
            ("valid", (count, n), np.uint8),
            ("status", (count,), np.intc),
            ("tokens", (count,), np.uintp),
        )
        if out is None:
            buffers = {name: np.empty(shape, dtype=dtype) for name, shape, dtype in specs}
            out = ArrayResult(**buffers, output_names=self.outputs, seed_names=self.seeds, copied_inputs=copied)
        else:
            if not isinstance(out, ArrayResult) or out.output_names != self.outputs or out.seed_names != self.seeds:
                raise ValueError("out must be an ArrayResult with matching output/seed metadata")
            for name, shape, dtype in specs:
                array = getattr(out, name)
                if not isinstance(array, np.ndarray) or array.shape != shape or array.dtype != np.dtype(dtype):
                    raise ValueError(f"out.{name} has the wrong shape or dtype")
                if not array.flags.c_contiguous or not array.flags.aligned or not array.flags.writeable:
                    raise ValueError(f"out.{name} must be aligned, writable and C-contiguous")
        output_arrays = [getattr(out, name) for name, _, _ in specs]
        for k, array in enumerate(output_arrays):
            if any(np.shares_memory(array, other) for other in output_arrays[:k]):
                raise ValueError("output buffers must not overlap")
            if any(other is not None and np.shares_memory(array, other) for other in arrays.values()):
                raise ValueError("output buffers must not overlap input arrays")
        dp = c.POINTER(c.c_double)
        def pointer(array, element=c.c_double):
            return None if array is None else array.ctypes.data_as(c.POINTER(element))
        code = self._batch(count, pointer(arrays["parameters"]), pointer(arrays["voltages"]),
            pointer(arrays["temperatures"]), self._limit(step_limit), out.values.ctypes.data_as(dp),
            out.jacobian.ctypes.data_as(dp), pointer(out.valid, c.c_uint8), pointer(out.status, c.c_int),
            pointer(out.tokens, c.c_size_t))
        if code:
            raise KernelError(code, 0)
        out.copied_inputs = copied
        return out
