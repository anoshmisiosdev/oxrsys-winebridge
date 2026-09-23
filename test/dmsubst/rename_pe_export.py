import struct, sys
src, dst, newname = sys.argv[1], sys.argv[2], sys.argv[3].encode()
b = bytearray(open(src, 'rb').read())
pe = struct.unpack_from('<I', b, 0x3c)[0]
nsec = struct.unpack_from('<H', b, pe + 6)[0]
optsz = struct.unpack_from('<H', b, pe + 20)[0]
opt = pe + 24
exp_rva = struct.unpack_from('<I', b, opt + 112)[0]  # PE32+ data dir 0
secs = pe + 24 + optsz
def rva2off(rva):
    for i in range(nsec):
        s = secs + 40 * i
        vsz, va, rsz, raw = struct.unpack_from('<IIII', b, s + 8)
        if va <= rva < va + max(vsz, rsz):
            return raw + rva - va
    raise SystemExit('rva not mapped')
name_off = rva2off(struct.unpack_from('<I', b, rva2off(exp_rva) + 12)[0])
old = bytes(b[name_off:b.index(0, name_off)])
assert len(newname) == len(old), (old, newname)
b[name_off:name_off + len(old)] = newname
open(dst, 'wb').write(b)
print(f'export name {old.decode()} -> {newname.decode()}')
