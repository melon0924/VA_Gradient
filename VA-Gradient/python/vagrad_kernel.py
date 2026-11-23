"""调用 emit-c 生成并编译的 C0 动态库；只依赖 Python 标准库。"""
from __future__ import annotations
import ctypes as c
from pathlib import Path
import math


class KernelError(RuntimeError):
    def __init__(self, code: int, token: int):
        self.code, self.token = code, token
        super().__init__(f"C kernel status={code}, expanded token={token}")


class Kernel:
    """参数为独立有效值；其他项使用编译时快照，不重算默认式。"""
    def __init__(self, library: str | Path):
        self.library = c.CDLL(str(Path(library).resolve(strict=True)))
        counts, names = {}, {}
        for kind in ("parameter", "node", "output", "seed"):
            count = getattr(self.library, f"vagrad_{kind}_count")
            count.argtypes, count.restype = [], c.c_size_t
            counts[kind] = count()
            name = getattr(self.library, f"vagrad_{kind}_name")
            name.argtypes, name.restype = [c.c_size_t], c.c_char_p
            names[kind] = tuple(name(k).decode("utf-8") for k in range(counts[kind]))
        self.parameters, self.nodes = names["parameter"], names["node"]
        self.outputs, self.seeds = names["output"], names["seed"]
        default = self.library.vagrad_default_parameter
        default.argtypes, default.restype = [c.c_size_t], c.c_double
        self.defaults = tuple(default(k) for k in range(counts["parameter"]))
        overridable = self.library.vagrad_parameter_overridable
        overridable.argtypes, overridable.restype = [c.c_size_t], c.c_int
        self.overridable = tuple(bool(overridable(k)) for k in range(counts["parameter"]))
        dp, bp, sp, ip = c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.POINTER(c.c_size_t), c.POINTER(c.c_int)
        self._eval = self.library.vagrad_eval
        self._eval.argtypes = [dp, dp, c.c_double, c.c_size_t, dp, dp, bp, sp]
        self._eval.restype = c.c_int
        self._batch = self.library.vagrad_eval_batch
        self._batch.argtypes = [c.c_size_t, dp, dp, dp, c.c_size_t, dp, dp, bp, ip, sp]
        self._batch.restype = c.c_int

    def _encode(self, *, parameters=None, voltages=None, temperature=None):
        parameters, voltages = parameters or {}, voltages or {}
        if set(parameters) - set(self.parameters):
            raise ValueError(f"unknown parameters: {set(parameters) - set(self.parameters)}")
        if set(voltages) - set(self.nodes):
            raise ValueError(f"unknown nodes: {set(voltages) - set(self.nodes)}")
        for k, name in enumerate(self.parameters):
            if name in parameters and not self.overridable[k]:
                raise ValueError(f"localparam cannot be overridden: {name}")
        # 未提供的节点/温度用 NaN 表示；内核实际读取时报告缺失输入。
        p = [float(parameters.get(name, self.defaults[k])) for k, name in enumerate(self.parameters)]
        v = [float(voltages.get(name, math.nan)) for name in self.nodes]
        return p, v, math.nan if temperature is None else float(temperature)

    @staticmethod
    def _limit(limit):
        if not isinstance(limit, int) or limit < 0 or limit > c.c_size_t(-1).value:
            raise ValueError("step_limit must fit an unsigned size_t")
        return limit

    def _result(self, values, jacobian, valid, row=0):
        n, lanes = len(self.outputs), len(self.seeds)
        return {
            "outputs": {name: values[row*n+k] for k, name in enumerate(self.outputs) if valid[row*n+k]},
            "jacobian": {name: {seed: jacobian[(row*n+k)*lanes+j] for j, seed in enumerate(self.seeds)}
                         for k, name in enumerate(self.outputs) if valid[row*n+k]},
        }

    def evaluate(self, *, step_limit=100000, **point):
        p, v, t = self._encode(**point)
        values = (c.c_double * len(self.outputs))()
        jacobian = (c.c_double * (len(self.outputs)*len(self.seeds)))()
        valid, token = (c.c_uint8 * len(self.outputs))(), c.c_size_t()
        code = self._eval((c.c_double*len(p))(*p), (c.c_double*len(v))(*v), t,
                          self._limit(step_limit), values, jacobian, valid, c.byref(token))
        if code:
            raise KernelError(code, token.value)
        return self._result(values, jacobian, valid)

    def evaluate_batch(self, points, *, step_limit=100000):
        encoded = [self._encode(**point) for point in points]
        count, n, lanes = len(encoded), len(self.outputs), len(self.seeds)
        p = [x for row, _, _ in encoded for x in row]
        v = [x for _, row, _ in encoded for x in row]
        temperatures = [t for _, _, t in encoded]
        values, jacobian = (c.c_double*(count*n))(), (c.c_double*(count*n*lanes))()
        valid, codes, tokens = (c.c_uint8*(count*n))(), (c.c_int*count)(), (c.c_size_t*count)()
        code = self._batch(count, (c.c_double*len(p))(*p), (c.c_double*len(v))(*v),
                           (c.c_double*count)(*temperatures), self._limit(step_limit),
                           values, jacobian, valid, codes, tokens)
        if code:
            raise KernelError(code, 0)
        return [self._result(values, jacobian, valid, row) if codes[row] == 0 else
                {"error": {"code": codes[row], "token": tokens[row]}} for row in range(count)]


if __name__ == "__main__":
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library")
    parser.add_argument("--param", action="append", default=[])
    parser.add_argument("--voltage", action="append", default=[])
    parser.add_argument("--temperature", type=float)
    args = parser.parse_args()
    def assignments(items):
        result = {}
        for item in items:
            name, value = item.split("=", 1)
            if name in result:
                raise ValueError(f"duplicate input: {name}")
            result[name] = float(value)
        return result
    result = Kernel(args.library).evaluate(parameters=assignments(args.param),
        voltages=assignments(args.voltage), temperature=args.temperature)
    print(json.dumps(result, ensure_ascii=False, allow_nan=False, indent=2))
