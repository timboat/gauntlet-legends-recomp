"""Add profiling marks to RSPRecomp's output (recomp/rsp/audio_ucode.cpp).

With RSP_PROF_LABEL defined (the Vita build does), every block of the audio microcode stores its RSP
address in rsp_prof_label, and the Vita's perf.txt "rsp:" line shows where the time goes. Without it
the marks compile to nothing.

usage: rsp_prof_marks.py <in.cpp> <out.cpp>   (in and out may be the same file)
"""
import re
import sys

HEADER = '''// Profiling (Vita perf.txt "rsp:" line): with RSP_PROF_LABEL defined, each block records its RSP address.
#ifdef RSP_PROF_LABEL
extern volatile uint16_t rsp_prof_label;
#define RSP_PROF_MARK(x) (rsp_prof_label = (x))
#else
#define RSP_PROF_MARK(x) ((void)0)
#endif
'''


def mark(src):
    if 'RSP_PROF_MARK' in src:
        return src
    src = src.replace('#include "librecomp/rsp_vu_impl.hpp"\n', '#include "librecomp/rsp_vu_impl.hpp"\n' + HEADER, 1)
    src = src.replace('    r1 = 0xFC0;\n', '    r1 = 0xFC0;\n    RSP_PROF_MARK(0x1000);\n', 1)
    return re.sub(r'^L_([0-9A-F]{4}):\n', lambda m: f'L_{m.group(1)}:\n    RSP_PROF_MARK(0x{m.group(1)});\n',
                  src, flags=re.M)


def main():
    with open(sys.argv[1], newline='') as f:
        raw = f.read()
    nl = '\r\n' if '\r\n' in raw else '\n'
    out = mark(raw.replace('\r\n', '\n')).replace('\n', nl)
    with open(sys.argv[2], 'w', newline='') as f:
        f.write(out)


if __name__ == '__main__':
    main()
