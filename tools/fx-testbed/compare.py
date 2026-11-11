"""Tabulate fxlab runs side by side:  python tools/fx-testbed/compare.py base zero new [--frame 510] [--metric sharp|mae|psnr]

Reads build/fxlab/out/<tag>.csv. Without --frame the captured frames are averaged.
sharp = edge energy of the output / the input's (1.00 = as crisp as the input); mae is in 8-bit steps.
"""
import csv, os, sys

def main():
    args = sys.argv[1:]
    frame, metrics, tags = None, ["sharp", "mae"], []
    i = 0
    while i < len(args):
        if args[i] == "--frame": frame = args[i + 1]; i += 2
        elif args[i] == "--metric": metrics = args[i + 1].split(","); i += 2
        else: tags.append(args[i]); i += 1
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "fxlab", "out")
    col = {"sharp": "sharp", "mae": "mae255", "psnr": "psnr"}
    data, probes, kinds = {}, [], {}
    for t in tags:
        with open(os.path.join(out, t + ".csv"), newline="") as f:
            for r in csv.DictReader(f):
                if frame and r["frame"] != frame: continue
                if r["probe"] not in probes: probes.append(r["probe"]); kinds[r["probe"]] = r["kind"]
                for m in metrics:
                    data.setdefault((t, r["probe"], m), []).append(float(r[col[m]]))
    for m in metrics:
        print(f"\n{m}" + (f" (frame {frame})" if frame else " (mean of captured frames)"))
        print(f"{'probe':<16}{'kind':<8}" + "".join(f"{t[:11]:>12}" for t in tags))
        for p in probes:
            row = f"{p:<16}{kinds[p]:<8}"
            for t in tags:
                v = data.get((t, p, m))
                row += f"{sum(v) / len(v):>12.3f}" if v else f"{'-':>12}"
            print(row)

if __name__ == "__main__":
    main()
