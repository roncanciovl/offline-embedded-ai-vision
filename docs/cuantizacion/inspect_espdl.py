"""Inspecciona un modelo .espdl de ESP-DL y muestra los datos de cuantización.

Reproduce las cifras usadas en las animaciones de docs/cuantizacion/:
número de parámetros, exponentes por tensor y por canal, histograma de pesos,
y el ejemplo de la primera convolución.

Uso:
    pip install flatbuffers numpy
    python docs/cuantizacion/inspect_espdl.py \
        Proyecto_Final_IA/esp-dl/models/obstacle_person_detect/models/p4/espdet_pico_224_224_obstacle_person.espdl

El formato está definido en Proyecto_Final_IA/esp-dl/esp-dl/fbs_loader/espdl.fbs.
El archivo empieza con una cabecera de 16 bytes ("EDL2", flags, tamaño, relleno)
seguida de un FlatBuffer cuyo root es `Model`. Los modelos cifrados no se pueden leer.
"""

import collections
import sys

import flatbuffers
import flatbuffers.number_types as N
import numpy as np
from flatbuffers.table import Table

# Índices de campo según espdl.fbs (orden de declaración en cada tabla)
MODEL_GRAPH = 7
GRAPH_NODE, GRAPH_INIT, GRAPH_INPUT, GRAPH_OUTPUT, GRAPH_VALUE_INFO = 0, 2, 4, 5, 6
NODE_INPUT, NODE_OUTPUT, NODE_OP = 0, 1, 3
TENSOR_DIMS, TENSOR_DTYPE, TENSOR_NAME, TENSOR_RAW, TENSOR_EXP = 0, 1, 6, 8, 13
VI_NAME, VI_EXP = 0, 3
DTYPES = {1: "float32", 2: "uint8", 3: "int8", 4: "uint16", 5: "int16", 6: "int32", 7: "int64"}
NP_DTYPES = {3: np.int8, 5: np.int16, 6: np.int32}


class Reader:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:4] != b"EDL2":
            sys.exit(f"{path}: no es un archivo .espdl (cabecera {data[:4]!r})")
        if int.from_bytes(data[4:8], "little") != 0:
            sys.exit(f"{path}: el modelo está cifrado; no se puede inspeccionar")
        self.size = len(data)
        self.buf = bytearray(data[16:])
        root = flatbuffers.encode.Get(N.UOffsetTFlags.packer_type, self.buf, 0)
        self.model = Table(self.buf, root)

    # --- acceso genérico a FlatBuffers ---
    @staticmethod
    def _off(t, i):
        return t.Offset(4 + 2 * i)

    def table(self, t, i):
        o = self._off(t, i)
        return Table(self.buf, t.Indirect(o + t.Pos)) if o else None

    def tables(self, t, i):
        o = self._off(t, i)
        if not o:
            return []
        start = t.Vector(o)
        return [Table(self.buf, t.Indirect(start + 4 * k)) for k in range(t.VectorLen(o))]

    def string(self, t, i):
        o = self._off(t, i)
        return t.String(o + t.Pos).decode() if o else None

    def strings(self, t, i):
        o = self._off(t, i)
        if not o:
            return []
        start = t.Vector(o)
        return [t.String(start + 4 * k).decode() for k in range(t.VectorLen(o))]

    def int64s(self, t, i):
        o = self._off(t, i)
        if not o:
            return []
        start = t.Vector(o)
        return [t.Get(N.Int64Flags, start + 8 * k) for k in range(t.VectorLen(o))]

    def int32(self, t, i):
        o = self._off(t, i)
        return t.Get(N.Int32Flags, o + t.Pos) if o else 0

    def raw(self, t):
        o = self._off(t, TENSOR_RAW)
        if not o:
            return b""
        start = t.Vector(o)
        return bytes(self.buf[start:start + 16 * t.VectorLen(o)])

    # --- tensores ---
    def tensor(self, t):
        dims = self.int64s(t, TENSOR_DIMS)
        dtype = self.int32(t, TENSOR_DTYPE)
        n = int(np.prod(dims)) if dims else 0
        values = None
        if dtype in NP_DTYPES:
            item = np.dtype(NP_DTYPES[dtype]).itemsize
            values = np.frombuffer(self.raw(t)[: n * item], dtype=NP_DTYPES[dtype]).reshape(dims)
        return {
            "name": self.string(t, TENSOR_NAME),
            "dims": dims,
            "dtype": DTYPES.get(dtype, str(dtype)),
            "exponents": self.int64s(t, TENSOR_EXP),
            "values": values,
        }


def main(path):
    r = Reader(path)
    g = r.table(r.model, MODEL_GRAPH)

    nodes = r.tables(g, GRAPH_NODE)
    ops = collections.Counter(r.string(n, NODE_OP) for n in nodes)
    print(f"Archivo: {path}  ({r.size:,} bytes)")
    print(f"Nodos: {len(nodes)}  ->  " + ", ".join(f"{k}: {v}" for k, v in ops.most_common()))

    inits = [r.tensor(t) for t in r.tables(g, GRAPH_INIT)]
    count = collections.Counter()
    for t in inits:
        count[t["dtype"]] += int(np.prod(t["dims"])) if t["dims"] else 0
    n8, n32 = count["int8"], count["int32"]
    print(f"\nParámetros: {n8:,} pesos int8 + {n32:,} bias int32 = {n8 + n32:,}")
    print(f"  como float32: {(n8 + n32) * 4:,} bytes")
    print(f"  como se guardan: {n8 + 4 * n32:,} bytes  ({(n8 + n32) * 4 / (n8 + 4 * n32):.2f}x menos)")

    print("\nEntradas / salidas (exponente):")
    for vi in r.tables(g, GRAPH_INPUT) + r.tables(g, GRAPH_OUTPUT):
        print(f"  {r.string(vi, VI_NAME):8s} e = {r.int64s(vi, VI_EXP)}")

    act_exp = collections.Counter(e for vi in r.tables(g, GRAPH_VALUE_INFO) for e in r.int64s(vi, VI_EXP))
    print("\nExponentes de activaciones (e: cantidad):", dict(sorted(act_exp.items())))
    w_exp = collections.Counter(e for t in inits if t["dtype"] == "int8" for e in t["exponents"])
    print("Exponentes de pesos por canal (e: cantidad):", dict(sorted(w_exp.items())))

    allw = np.concatenate([t["values"].ravel() for t in inits if t["dtype"] == "int8"]).astype(int)
    hist = np.histogram(allw, bins=32, range=(-128, 128))[0]
    print("\nHistograma de todos los pesos int8 (32 bins en [-128, 128)):")
    print(" ", hist.tolist())
    print(f"  |q| >= 64 (saturaría con e-1): {np.mean(np.abs(allw) >= 64):.1%}")
    print(f"  q = -128 o 127: {np.mean((allw == -128) | (allw == 127)):.3%}")

    by_name = {t["name"]: t for t in inits}
    w0, b0 = by_name.get("model.0.conv.weight"), by_name.get("model.0.conv.bias")
    if w0 is not None and b0 is not None:
        w = w0["values"].astype(int)  # layout HWIO: [kh, kw, cin, cout]
        print("\nPrimera capa (model.0.conv):", w0["dims"])
        print("  exponentes por canal de salida:", w0["exponents"])
        print("  exponentes del bias:           ", b0["exponents"])
        print("  |w| máximo por canal (float):  ",
              [round(float(np.abs(w[..., c]).max() * 2.0 ** w0["exponents"][c]), 3) for c in range(w.shape[-1])])
        print("  filtro 0, canal R:", w[:, :, 0, 0].tolist())
        print("  bias filtro 0:", int(b0["values"][0]))

        # Ejemplo de las animaciones: parche gris (R = G = B), entrada con e = -7, salida con e = -5
        pix = np.array([[200, 196, 190], [204, 198, 180], [208, 202, 186]])
        q = np.clip(np.round(pix / 255 * 128), -128, 127).astype(int)
        parts = [int((q * w[:, :, c, 0]).sum()) for c in range(w.shape[2])]
        acc = sum(parts) + int(b0["values"][0])
        shift = -5 - (-7 + w0["exponents"][0])
        out = int(np.clip(np.round(acc / 2 ** shift), 0, 127))  # np.round = ROUND_HALF_EVEN; ReLU -> >= 0
        print(f"  ejemplo: q = {q.tolist()}  sumas por canal = {parts}  acc = {acc}  >> {shift} -> {out}"
              f"  ({out} x 2^-5 = {out * 2 ** -5})")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
