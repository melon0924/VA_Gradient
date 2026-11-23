"""四组偏置 -> NumPy 原值/雅可比，并保存无需 pickle 的 NPZ。"""
import argparse
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from vagrad_numpy import NumpyKernel

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("library")
parser.add_argument("--output")
args = parser.parse_args()
model = NumpyKernel(args.library)
gates = np.array([.3, .7, 1., 1.2])
voltages = np.empty((gates.size, len(model.nodes)), dtype=np.float64)
for name, values in {"g": gates, "d": .05, "s": 0}.items():
    voltages[:, model.nodes.index(name)] = values
result = model.evaluate_arrays(voltages=voltages, temperatures=np.full(gates.size, 300.)).require_all()
print("Vg,ids,d_ids_d_G0,d_ids_d_VTH0")
for row in zip(gates, result.observation("ids"), result.derivative("ids", "G0"), result.derivative("ids", "VTH0")):
    print(",".join(format(float(value), ".17g") for value in row))
if args.output:
    np.savez(args.output, values=result.values, jacobian=result.jacobian, valid=result.valid,
             status=result.status, tokens=result.tokens, outputs=np.asarray(model.outputs),
             seeds=np.asarray(model.seeds), gates=gates)
