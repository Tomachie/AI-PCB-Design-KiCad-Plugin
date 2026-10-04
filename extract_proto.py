import sys, io

data = open(r"C:\Program Files\KiCad\10.0\bin\kiapi.dll", "rb").read()

def rv(b, i):                      # read varint
    r = s = 0
    while True:
        if i >= len(b): raise ValueError("eof")
        c = b[i]; i += 1
        r |= (c & 0x7F) << s
        if not (c & 0x80): return r, i
        s += 7
        if s > 63: raise ValueError("varint too long")

def fields(b):                     # yield (num, wire, value) over a whole buffer
    i = 0
    while i < len(b):
        tag, i = rv(b, i)
        num, wt = tag >> 3, tag & 7
        if num == 0: raise ValueError("field 0")
        if wt == 0:   v, i = rv(b, i)
        elif wt == 2:
            ln, i = rv(b, i)
            if i + ln > len(b): raise ValueError("len overflow")
            v = b[i:i+ln]; i += ln
        elif wt == 5: v = b[i:i+4]; i += 4
        elif wt == 1: v = b[i:i+8]; i += 8
        else: raise ValueError("wire %d" % wt)
        yield num, wt, v

def sub(b):
    d = {}
    for n, w, v in fields(b):
        d.setdefault(n, []).append(v)
    return d

TYPES = {1:"double",2:"float",3:"int64",4:"uint64",5:"int32",6:"fixed64",7:"fixed32",
         8:"bool",9:"string",10:"group",11:"message",12:"bytes",13:"uint32",14:"enum",
         15:"sfixed32",16:"sfixed64",17:"sint32",18:"sint64"}
LABELS = {1:"optional",2:"required",3:"repeated"}

def msg(d, indent, out):
    name = d.get(1,[b""])[0].decode()
    out.write("%smessage %s {\n" % (indent, name))
    for f in d.get(2, []):
        fd = sub(f)
        fname = fd.get(1,[b""])[0].decode()
        fnum  = fd.get(3,[0])[0]
        ftype = TYPES.get(fd.get(5,[0])[0], "?")
        flab  = LABELS.get(fd.get(4,[0])[0], "")
        tname = fd.get(6,[b""])[0].decode() if 6 in fd else ""
        t = tname if tname else ftype
        lab = "repeated " if flab == "repeated" else ""
        out.write("%s  %s%s %s = %d;\n" % (indent, lab, t, fname, fnum))
    for n in d.get(3, []):          # nested_type
        msg(sub(n), indent + "  ", out)
    for e in d.get(4, []):          # enum_type
        enum(sub(e), indent + "  ", out)
    out.write("%s}\n" % indent)

def enum(d, indent, out):
    out.write("%senum %s {\n" % (indent, d.get(1,[b""])[0].decode()))
    for v in d.get(2, []):
        vd = sub(v)
        out.write("%s  %s = %d;\n" % (indent, vd.get(1,[b""])[0].decode(), vd.get(2,[0])[0]))
    out.write("%s}\n" % indent)

wanted = sys.argv[1:] if len(sys.argv) > 1 else [
    "common/envelope.proto", "common/commands/base_commands.proto",
    "common/commands/project_commands.proto", "common/types/base_types.proto",
    "schematic/schematic_commands.proto",
]

for name in wanted:
    nb = name.encode()
    pat = b"\x0a" + bytes([len(nb)]) + nb
    idx = data.find(pat)
    if idx < 0:
        print("!! not found:", name); continue
    # parse greedily from here, stopping when a second file name appears
    best = None
    for end in range(len(data) - idx, 0, -1):
        pass
    buf = data[idx:idx+200000]
    out = io.StringIO()
    out.write("=" * 70 + "\n// %s\n" % name + "=" * 70 + "\n")
    i = 0; seen_name = False
    try:
        while i < len(buf):
            tag, j = rv(buf, i)
            num, wt = tag >> 3, tag & 7
            if wt != 2 and num not in (12,):
                if wt == 0:
                    _, j = rv(buf, j); i = j; continue
                break
            ln, j = rv(buf, j)
            if j + ln > len(buf): break
            val = buf[j:j+ln]; i = j + ln
            if num == 1:
                if seen_name: break
                seen_name = True
                out.write("// file: %s\n" % val.decode('utf-8','replace'))
            elif num == 2: out.write("package %s;\n" % val.decode('utf-8','replace'))
            elif num == 3: out.write("import \"%s\";\n" % val.decode('utf-8','replace'))
            elif num == 4: msg(sub(val), "", out)
            elif num == 5: enum(sub(val), "", out)
            elif num == 12: out.write("syntax = \"%s\";\n" % val.decode('utf-8','replace'))
    except Exception as e:
        out.write("// (stopped: %s)\n" % e)
    print(out.getvalue())
