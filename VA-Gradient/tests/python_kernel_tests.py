import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from vagrad_kernel import Kernel, KernelError

kernel = Kernel(sys.argv.pop(1))
control = Kernel(sys.argv.pop(1)) if len(sys.argv) > 1 else None
bias = {"d": .05, "g": 1, "s": 0}


class KernelTests(unittest.TestCase):
    def test_point(self):
        result = kernel.evaluate(voltages=bias, temperature=300)
        self.assertAlmostEqual(result["outputs"]["ids"], 2.625e-5, places=16)
        self.assertAlmostEqual(result["jacobian"]["ids"]["G0"], .02625, places=14)
        self.assertAlmostEqual(result["jacobian"]["ids"]["VTH0"], -5e-5, places=16)

    def test_batch_error_isolation(self):
        result = kernel.evaluate_batch([{"voltages": bias, "temperature": t} for t in (300, 0, 600)])
        self.assertEqual(result[1]["error"]["code"], 3)
        self.assertAlmostEqual(result[2]["outputs"]["flow:ds"], result[0]["outputs"]["flow:ds"]/2, places=16)
        self.assertEqual(kernel.evaluate_batch([]), [])

    def test_inputs(self):
        with self.assertRaises(ValueError):
            kernel.evaluate(parameters={"unknown": 1})
        with self.assertRaises(ValueError):
            kernel.evaluate(voltages={"unknown": 1})
        with self.assertRaises(ValueError):
            kernel.evaluate(step_limit=-1)
        with self.assertRaises(KernelError) as error:
            kernel.evaluate(parameters={"G0": -1}, voltages=bias, temperature=300)
        self.assertEqual(error.exception.code, 2)
        self.test_point()

    def test_localparam_and_undefined(self):
        if control is None:
            self.skipTest("control fixture unavailable")
        result = control.evaluate(parameters={"EN": 0})
        self.assertNotIn("ids", result["outputs"])
        result = control.evaluate(parameters={"EN": 1, "P": 4})
        self.assertEqual(result["outputs"]["ids"], 16)
        self.assertEqual(result["jacobian"]["ids"]["P"], 8)
        with self.assertRaises(ValueError):
            control.evaluate(parameters={"FROZEN": 5})


if __name__ == "__main__":
    unittest.main()
