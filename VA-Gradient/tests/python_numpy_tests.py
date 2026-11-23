import sys
from pathlib import Path
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from vagrad_numpy import NumpyKernel, ArrayResult
from vagrad_kernel import KernelError

model = NumpyKernel(sys.argv.pop(1))
control = NumpyKernel(sys.argv.pop(1))


def inputs():
    voltages = np.empty((4, len(model.nodes)))
    for name, values in {"d": .05, "g": [.3, .7, 1, 1.2], "s": 0}.items():
        voltages[:, model.nodes.index(name)] = values
    return voltages, np.full(4, 300.)


class NumpyTests(unittest.TestCase):
    def test_values_and_zero_copy(self):
        v, t = inputs()
        result = model.evaluate_arrays(voltages=v, temperatures=t).require_all()
        self.assertEqual(result.values.shape, (4, len(model.outputs)))
        self.assertEqual(result.jacobian.shape, (4, len(model.outputs), len(model.seeds)))
        self.assertFalse(any(result.copied_inputs.values()))
        np.testing.assert_allclose(result.observation("ids"), [5e-14, 1.125e-5, 2.625e-5, 3.625e-5], rtol=1e-14)
        np.testing.assert_allclose(result.derivative("ids", "G0"), [0, .01125, .02625, .03625], rtol=1e-14)
        np.testing.assert_array_equal(v[:, model.nodes.index("g")], [.3, .7, 1, 1.2])
        reused = model.evaluate_arrays(voltages=v, temperatures=t, out=result)
        self.assertIs(reused, result)
        v.flags.writeable = False
        model.evaluate_arrays(voltages=v, temperatures=t).require_all()

    def test_errors_and_copy(self):
        v, t = inputs()
        noncontiguous = np.zeros((4, 2*len(model.nodes)))[:, ::2]
        noncontiguous[:] = v
        with self.assertRaises(ValueError):
            model.evaluate_arrays(voltages=noncontiguous, temperatures=t)
        result = model.evaluate_arrays(voltages=noncontiguous, temperatures=t, copy=True).require_all()
        self.assertTrue(result.copied_inputs["voltages"])
        unaligned = np.ndarray(v.shape, dtype=np.float64, buffer=np.empty(v.nbytes+1, dtype=np.uint8), offset=1)
        unaligned[:] = v
        with self.assertRaises(ValueError):
            model.evaluate_arrays(voltages=unaligned, temperatures=t)
        model.evaluate_arrays(voltages=unaligned, temperatures=t, copy=True).require_all()
        with self.assertRaises(ValueError):
            model.evaluate_arrays(voltages=v.astype(np.float32), temperatures=t)
        with self.assertRaises(ValueError):
            model.evaluate_arrays(voltages=v, temperatures=t[:3])
        with self.assertRaises(TypeError):
            model.evaluate_arrays(voltages=v.tolist(), temperatures=t)
        t[1] = 0
        result = model.evaluate_arrays(voltages=v, temperatures=t)
        np.testing.assert_array_equal(result.status, [0, 3, 0, 0])
        self.assertTrue(np.isnan(result.values[1]).all())
        with self.assertRaises(KernelError):
            result.require_all()
        empty = model.evaluate_arrays(count=0)
        self.assertEqual(empty.values.shape[0], 0)
        borrowed_empty = model.evaluate_arrays(temperatures=np.empty(0), copy=True)
        self.assertFalse(borrowed_empty.copied_inputs["temperatures"])

    def test_output_alias_and_localparam(self):
        v, t = inputs()
        result = model.evaluate_arrays(voltages=v, temperatures=t)
        result.values = result.jacobian.reshape(-1)[:result.values.size].reshape(result.values.shape)
        with self.assertRaisesRegex(ValueError, "must not overlap"):
            model.evaluate_arrays(voltages=v, temperatures=t, out=result)
        valid = control.evaluate_arrays(count=2).require_all()
        self.assertTrue(np.isnan(valid.observation("ids")).all())
        p = np.tile(np.asarray(control.defaults), (2, 1))
        p[:, control.parameters.index("FROZEN")] = 5
        with self.assertRaises(ValueError):
            control.evaluate_arrays(parameters=p, count=2)


if __name__ == "__main__":
    unittest.main()
