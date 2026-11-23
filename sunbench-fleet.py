#!/usr/bin/env python3
# Copyright (C) 2026 thenetworkinglab
# SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE)
"""sunbench-fleet.py -- combine sunbench --log files into one fleet HTML page.

    ./sunbench-fleet.py logs/ -o fleet.html
    ./sunbench-fleet.py ss5.log ultra10.log sunblade*.log -o fleet.html

Stateless: the .log files are the database.  Every invocation re-parses
everything it is given and rewrites the whole page.  Directories are scanned
for *.log.  A machine is identified by its hostname (from the "system :"
line); when a machine appears in several runs or files, the LATEST run of
each module wins (by the run timestamp, falling back to parse order).

Runs on the modern machine -- Python 3, stdlib only.
"""

import argparse
import html
import os
import re
import sys
from datetime import datetime

# ---------------------------------------------------------------- parsing

SYN_KEYS = ("system", "model", "cpu", "memory", "disk", "drive", "net", "note")

MODULES = {
    "disk": "disk",
    "disk block-size sweep": "disksweep",
    "cpu": "cpu",
    "memory (working-set sweep)": "mem",
    "network": "net",
}

CPU_ROWS = ("int alu", "int muldiv", "fp double", "saxpy sp", "daxpy dp",
            "saxpy mem", "daxpy mem", "dhrystone", "whetstone")


def parse_date(s):
    try:
        return datetime.strptime(s.strip(), "%a %b %d %H:%M:%S %Y")
    except ValueError:
        return None


def floats(line):
    return [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", line)]


def parse_section(lines, order):
    """One '====' delimited section -> dict or None."""
    sec = {"order": order, "syn": {}, "module": None, "data": {}}
    it = iter(range(len(lines)))
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r"sunbench (\S+)\s+run: (.+)$", ln.strip())
        if m:
            sec["version"] = m.group(1)
            sec["date"] = parse_date(m.group(2))
            sec["datestr"] = m.group(2).strip()
        m = re.match(r"\s{2}(\w+)\s*: (.*)$", ln)
        if m and m.group(1) in SYN_KEYS and sec["module"] is None:
            sec["syn"][m.group(1)] = m.group(2).strip()
        m = re.match(r"sunbench -- (.+)$", ln.strip())
        if m:
            name = m.group(1).strip()
            if name in MODULES:
                sec["module"] = MODULES[name]
            elif name.startswith("cpu (MP"):
                sec["module"] = "cpump"
            elif name.startswith("memory (MP"):
                sec["module"] = "memmp"
            else:
                return None
            m2 = re.search(r"MP: (\d+) process", name)
            sec["mp_ncpus"] = int(m2.group(1)) if m2 else None
            body = lines[i + 1:]
            sec["data"] = PARSERS[sec["module"]](body)
            break
        i += 1
    if sec["module"] is None or not sec["syn"].get("system"):
        return None
    parts = sec["syn"]["system"].split()
    sec["host"] = parts[1]
    # key includes the OS name and release, so one machine benchmarked
    # under several OSes (the SS2: SunOS 4.1.4 / Solaris 2.3 / 2.6) gets
    # one card per OS instead of latest-wins across eras -- and non-Sun
    # ports (NetBSD) are labelled as themselves, not as SunOS
    sec["osname"] = parts[0] if parts else "?"
    sec["oskey"] = parts[2] if len(parts) > 2 else "?"
    sec["host"] = sec["host"] + " (" + sec["osname"] + " " + sec["oskey"] + ")"
    return sec


def p_disk(body):
    d = {"tests": {}, "pctl": []}
    for ln in body:
        m = re.match(r"\s+(seq write|seq read|rand read|rand write)\s+"
                     r"([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", ln)
        if m:
            d["tests"][m.group(1)] = tuple(float(m.group(k)) for k in (2, 3, 4))
        if re.match(r"\s+rand (read|write)\s+min ", ln):
            d["pctl"].append(ln.strip())
        for key in ("method", "file"):
            m2 = re.match(r"\s{2}%s\s*: (.*)$" % key, ln)
            if m2:
                d[key] = m2.group(1).strip()
    return d


def p_disksweep(body):
    d = {"rows": []}
    for ln in body:
        v = floats(ln)
        if len(v) == 6 and re.match(r"\s+\d+\s", ln):
            d["rows"].append(v)      # blk, seqW, seqR, randR, randW, iops
    return d


def p_cpu(body):
    d = {"rows": {}}
    for ln in body:
        for key in ("build", "clock"):
            m = re.match(r"\s{2}%s\s*: (.*)$" % key, ln)
            if m:
                d[key] = m.group(1).strip()
        for name in CPU_ROWS:
            m = re.match(r"\s{2}%s\s+([\d.]+) (\S+)(?:\s+(.*))?$"
                         % re.escape(name), ln)
            if m:
                d["rows"][name] = {"val": float(m.group(1)),
                                   "unit": m.group(2),
                                   "extra": (m.group(3) or "").strip()}
    m = re.search(r"([\d.]+)", d.get("clock", ""))
    d["mhz"] = float(m.group(1)) if m else None
    return d


def p_mem(body):
    d = {"rows": []}
    for ln in body:
        v = floats(ln)
        if len(v) == 6 and re.match(r"\s+\d+\s", ln):
            d["rows"].append(v)      # setKB, read, read8, write, copy, lat
    return d


def p_net(body):
    d = {"tcp": {}, "wsweep": [], "rsweep": [], "udpup": [], "udpdown": []}
    cur = None
    for ln in body:
        m = re.match(r"\s+server : (.*)$", ln)
        if m:
            d["server"] = m.group(1).strip()
        m = re.match(r"\s+(TCP|UDP) round trip\s+([\d.]+) ms", ln)
        if m:       # UDP: SBDOS (DOS, no TCP) times datagram echoes
            d["tcp"]["rtt"] = float(m.group(2))
            d["tcp"]["rtt_proto"] = m.group(1)
        m = re.match(r"\s+TCP (upload|download) \(([^)]*)\)\s+([\d.]+) Mbit/s", ln)
        if m:
            d["tcp"][m.group(1)] = float(m.group(3))
            d["tcp"][m.group(1) + "_how"] = m.group(2)
        if "write-size sweep" in ln:
            cur = "wsweep"
        elif "read-size sweep" in ln:
            cur = "rsweep"
        elif "UDP upload sweep" in ln:
            cur = "udpup"
        elif "UDP download sweep" in ln:
            cur = "udpdown"
        elif cur:
            v = floats(ln)
            if re.match(r"\s+\d+\s+[\d.]+", ln) and len(v) >= 2:
                d[cur].append(v)     # bytes, Mbit [, loss%]
    return d


def p_cpump(body):
    """cpu MP rows:  name   1-proc   N-proc   speedup x  scaling %  unit"""
    d = {"rows": []}
    for ln in body:
        m = re.match(r"\s{2}(.+?)\s{2,}([\d.]+)\s+([\d.]+)\s+"
                     r"([\d.]+)x\s+([\d.]+)%\s+(\S+)\s*$", ln)
        if m:
            d["rows"].append({"name": m.group(1).strip(),
                              "single": float(m.group(2)),
                              "agg": float(m.group(3)),
                              "speedup": float(m.group(4)),
                              "scaling": float(m.group(5)),
                              "unit": m.group(6)})
    return d


def p_memmp(body):
    """mem MP rows:  name   x.x MB/s   y.y MB/s   s.ssx   e.e% (or ns / -)"""
    d = {"rows": []}
    for ln in body:
        m = re.match(r"\s{2}(.+?)\s{2,}([\d.]+) (MB/s|ns)\s+"
                     r"([\d.]+) (?:MB/s|ns)\s+([\d.]+)x\s+(\S+)\s*$", ln)
        if m:
            sc = m.group(6).rstrip("%")
            d["rows"].append({"name": m.group(1).strip(),
                              "single": float(m.group(2)),
                              "agg": float(m.group(4)),
                              "speedup": float(m.group(5)),
                              "scaling": float(sc) if sc != "-" else None,
                              "unit": m.group(3)})
    return d


PARSERS = {"disk": p_disk, "disksweep": p_disksweep, "cpu": p_cpu,
           "mem": p_mem, "net": p_net, "cpump": p_cpump, "memmp": p_memmp}


def load(paths):
    """Parse all logs -> {host: {syn, date, modules:{name:section}}}."""
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(os.path.join(p, f) for f in os.listdir(p)
                            if f.endswith(".log"))
        else:
            files.append(p)
    machines = {}
    order = 0
    for path in files:
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError as e:
            print("skip %s: %s" % (path, e), file=sys.stderr)
            continue
        for chunk in re.split(r"^={20,}\s*$", text, flags=re.M):
            lines = chunk.splitlines()
            if not any("sunbench" in ln for ln in lines):
                continue
            order += 1
            sec = parse_section(lines, order)
            if sec is None:
                continue
            sec["path"] = os.path.basename(path)
            mach = machines.setdefault(sec["host"], {"modules": {}, "nruns": 0})
            mach["nruns"] += 1
            key = (sec["date"] or datetime.min, sec["order"])
            old = mach["modules"].get(sec["module"])
            if old is None or key >= old["_key"]:
                sec["_key"] = key
                mach["modules"][sec["module"]] = sec
            if "syn" not in mach or key >= mach["_synkey"]:
                mach["syn"] = sec["syn"]
                mach["datestr"] = sec.get("datestr", "")
                mach["version"] = sec.get("version", "?")
                mach["_synkey"] = key
    return machines


# ---------------------------------------------------------------- charts

# first six match src/svgchart.c so fleet charts feel like the reports
PALETTE = ["#5661d6", "#2e9e63", "#d97706", "#d64545", "#8b5cf6", "#0e7490",
           "#be185d", "#65a30d", "#475569", "#c026d3", "#a16207", "#0f766e"]

CW, CH = 860, 340
ML, MR, MT, MB = 66, 16, 34, 44


def nice_pow10(v, up):
    import math
    if v <= 0:
        return 1.0
    e = math.log10(v)
    return 10.0 ** (math.ceil(e) if up else math.floor(e))


def fmt_tick(v):
    if v >= 1e9:
        return "%gG" % (v / 1e9)
    if v >= 1e6:
        return "%gM" % (v / 1e6)
    if v >= 1000:
        return "%gK" % (v / 1000.0)
    if v >= 1:
        return "%g" % v
    return "%g" % v


def svg_chart(title, yunit, xlabel, xlabs, series, logy=True):
    """series: list of (name, color, {xlab: value})"""
    import math
    pts = [v for _, _, d in series for v in d.values() if v is not None and v > 0]
    if not pts or len(xlabs) < 2:
        return ""
    lo, hi = min(pts), max(pts)
    if logy:
        ylo, yhi = nice_pow10(lo, False), nice_pow10(hi, True)
        if ylo == yhi:
            yhi *= 10
        def ypix(v):
            f = (math.log10(v) - math.log10(ylo)) / (math.log10(yhi) - math.log10(ylo))
            return MT + (CH - MT - MB) * (1 - f)
        ticks = []
        t = ylo
        while t <= yhi * 1.0001:
            ticks.append(t)
            t *= 10
    else:
        ylo, yhi = 0.0, hi * 1.05
        def ypix(v):
            return MT + (CH - MT - MB) * (1 - (v - ylo) / (yhi - ylo))
        ticks = [ylo + (yhi - ylo) * i / 5.0 for i in range(6)]

    def xpix(i):
        return ML + (CW - ML - MR) * i / float(len(xlabs) - 1)

    out = []
    a = out.append
    a('<svg viewBox="0 0 %d %d" xmlns="http://www.w3.org/2000/svg" '
      'width="100%%" style="margin-top:8px;display:block;max-width:%dpx">'
      % (CW, CH, CW))
    a('<text x="%d" y="20" font-family="sans-serif" font-size="13" '
      'font-weight="bold" fill="#1a2030">%s <tspan fill="#8a92a6" '
      'font-weight="normal">(%s)</tspan></text>' % (ML, html.escape(title),
                                                    html.escape(yunit)))
    for n, t in enumerate(ticks):
        y = ypix(t)
        base = (n == 0)
        a('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>' %
          (ML, y, CW - MR, y, "#9aa1b5" if base else "#e7eaf2"))
        a('<text x="%d" y="%.1f" font-family="sans-serif" font-size="10" '
          'fill="#5a6478" text-anchor="end">%s</text>' % (ML - 6, y + 3,
                                                          fmt_tick(t)))
    step = max(1, len(xlabs) // 16)
    for i, xl in enumerate(xlabs):
        if i % step and i != len(xlabs) - 1:
            continue
        x = xpix(i)
        a('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#f0f2f7"/>' %
          (x, MT, x, CH - MB))
        a('<text x="%.1f" y="%d" font-family="sans-serif" font-size="10" '
          'fill="#5a6478" text-anchor="middle">%s</text>' %
          (x, CH - MB + 16, html.escape(xl)))
    a('<text x="%d" y="%d" font-family="sans-serif" font-size="10" '
      'fill="#8a92a6" text-anchor="middle">%s</text>' %
      ((ML + CW - MR) // 2, CH - 6, html.escape(xlabel)))
    for name, color, d in series:
        seg = []
        for i, xl in enumerate(xlabs):
            v = d.get(xl)
            if v is None or v <= 0:
                if len(seg) > 1:
                    a('<polyline points="%s" fill="none" stroke="%s" '
                      'stroke-width="2"/>' % (" ".join(seg), color))
                seg = []
                continue
            seg.append("%.1f,%.1f" % (xpix(i), ypix(v)))
        if len(seg) > 1:
            a('<polyline points="%s" fill="none" stroke="%s" '
              'stroke-width="2"/>' % (" ".join(seg), color))
        for i, xl in enumerate(xlabs):
            v = d.get(xl)
            if v is not None and v > 0:
                a('<circle cx="%.1f" cy="%.1f" r="2.6" fill="%s"/>' %
                  (xpix(i), ypix(v), color))
    a('</svg>')
    leg = ['<div class="legend">']
    for name, color, _ in series:
        leg.append('<span><i style="background:%s"></i>%s</span>' %
                   (color, html.escape(name)))
    leg.append('</div>')
    return "".join(out) + "".join(leg)


def sizelab(kb):
    kb = int(kb)
    return "%dM" % (kb // 1024) if kb >= 1024 else "%dK" % kb


# ---------------------------------------------------------------- page

# Matches the look of the reports written by src/html.c (Sun purple #5661d6).
CSS = """
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;
     background:#eef0f4;color:#1a2030;padding:24px;line-height:1.45}
.wrap{max-width:960px;margin:0 auto}
h1{font-size:22px;font-weight:700;letter-spacing:-.01em}
h1 .v{color:#5661d6;font-weight:600}
.sub{color:#5a6478;font-size:13px;margin:2px 0 18px}
.card{background:#fff;border-radius:10px;padding:18px 20px;margin:0 0 16px;
      box-shadow:0 1px 3px rgba(20,24,40,.08)}
h2{font-size:15px;font-weight:650;margin:0 0 2px}
h3{font-size:13px;font-weight:650;margin:14px 0 2px;color:#1a2030;
   text-transform:uppercase;letter-spacing:.04em}
.meta,.small{color:#5a6478;font-size:12px;margin:0 0 10px}
table{border-collapse:collapse;width:100%;font-size:13px;
      font-variant-numeric:tabular-nums;margin:4px 0 10px}
th{text-align:right;font-weight:600;color:#5a6478;font-size:11px;
   text-transform:uppercase;letter-spacing:.04em;padding:6px 10px;
   border-bottom:1px solid #e3e6ee}
th.l,td.l{text-align:left}
td{text-align:right;padding:5px 10px;border-bottom:1px solid #f0f2f7}
tr:last-child td{border-bottom:0}
.grid{display:table;width:100%;font-size:13px}
.grid .row{display:table-row}
.grid .k{display:table-cell;color:#5a6478;padding:2px 14px 2px 0;
         white-space:nowrap;width:1%}
.grid .val{display:table-cell;padding:2px 0}
.legend{font-size:12px;color:#5a6478;margin:4px 0 18px}
.legend span{margin-right:14px;white-space:nowrap}
.legend i{display:inline-block;width:10px;height:10px;margin-right:4px;
          border-radius:2px}
.dot{display:inline-block;width:9px;height:9px;border-radius:2px;
     margin-right:6px}
.foot{color:#8a92a6;font-size:11px;margin-top:14px}
a{color:#5661d6;text-decoration:none}
a:hover{text-decoration:underline}
@media print{body{background:#fff;padding:0}
.card{box-shadow:none;border:1px solid #d5d9e4;page-break-inside:avoid}}
"""


def esc(s):
    return html.escape(str(s))


def fnum(v, prec=1):
    if v is None:
        return "&#8211;"
    if v >= 10000:
        return "{:,.0f}".format(v)
    return ("%%.%df" % prec) % v


def summary_row(host, mach, color):
    syn, mods = mach["syn"], mach["modules"]
    cpu = mods.get("cpu", {}).get("data", {})
    mem = mods.get("mem", {}).get("data", {})
    dsk = mods.get("disk", {}).get("data", {})
    net = mods.get("net", {}).get("data", {})
    rows = cpu.get("rows", {})
    vax = None
    if "dhrystone" in rows:
        m = re.search(r"([\d.]+) VAX-MIPS", rows["dhrystone"]["extra"])
        vax = float(m.group(1)) if m else None
    mrows = mem.get("rows", [])
    dram_rd = mrows[-1][1] if mrows else None
    dram_lat = mrows[-1][5] if mrows else None
    seqr = dsk.get("tests", {}).get("seq read", (None,))[0]
    tcpup = net.get("tcp", {}).get("upload")
    tcpdn = net.get("tcp", {}).get("download")
    sysw = syn.get("system", "").split()
    os_short = (sysw[0] + " " + sysw[2]) if len(sysw) > 2 else ""
    return (vax or 0, """
<tr><td class="l"><i class="dot" style="background:%s"></i><a href="#%s">%s</a></td>
<td class="l">%s</td><td>%s</td><td class="l">%s</td>
<td class="l">%s</td>
<td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>""" % (
        color, esc(host), esc(host),
        esc(syn.get("model", "").replace("SUNW,", "").replace("Sun Microsystems ", "")),
        (esc(cpu["clock"].replace(" MHz", "")) if cpu.get("clock") else "&#8211;"),
        esc(os_short), esc(syn.get("memory", "")),
        fnum(vax, 1),
        fnum(rows.get("whetstone", {}).get("val")),
        fnum(rows.get("daxpy dp", {}).get("val"), 1),
        fnum(dram_rd), fnum(dram_lat, 0),
        fnum(seqr, 1),
        ("%s / %s" % (fnum(tcpup, 1), fnum(tcpdn, 1)))
        if tcpup is not None else "&#8211;"))


def mp_table(a, sec, title):
    n = sec.get("mp_ncpus")
    a("<h3>%s</h3>" % title)         # title is trusted HTML (contains &mdash;)
    if n:
        a('<p class="meta">%d concurrent processes</p>' % n)
    a("<table><tr><th class='l'>test</th><th>1-proc</th><th>N-proc</th>"
      "<th>speedup</th><th>scaling</th></tr>")
    for r in sec["data"]["rows"]:
        a("<tr><td class='l'>%s</td><td>%s %s</td><td>%s %s</td>"
          "<td>%.2f&times;</td><td>%s</td></tr>" %
          (esc(r["name"]), fnum(r["single"], 1), esc(r["unit"]),
           fnum(r["agg"], 1), esc(r["unit"]), r["speedup"],
           "&mdash;" if r["scaling"] is None else "%.1f%%" % r["scaling"]))
    a("</table>")


def module_tables(mach):
    out = []
    a = out.append
    mods = mach["modules"]
    if "cpu" in mods:
        c = mods["cpu"]["data"]
        a("<h3>cpu</h3>")
        a('<p class="small">%s &#8212; %s</p>' %
          (esc(c.get("build", "")), esc(c.get("clock", ""))))
        a("<table><tr><th class='l'>kernel</th><th>rate</th>"
          "<th class='l'>&nbsp;</th></tr>")
        for name in CPU_ROWS:
            r = c["rows"].get(name)
            if r:
                a("<tr><td class='l'>%s</td><td>%s %s</td>"
                  "<td class='l'>%s</td></tr>" %
                  (esc(name), fnum(r["val"], 3 if r["val"] < 100 else 0),
                   esc(r["unit"]), esc(r["extra"])))
        a("</table>")
    if "cpump" in mods:
        mp_table(a, mods["cpump"], "cpu &mdash; MP scaling")
    if "mem" in mods:
        a("<h3>memory</h3>")
        a("<table><tr><th>set(KB)</th><th>read</th><th>read8</th>"
          "<th>write</th><th>copy</th><th>lat(ns)</th></tr>")
        for r in mods["mem"]["data"]["rows"]:
            a("<tr><td>%d</td>%s</tr>" %
              (int(r[0]), "".join("<td>%s</td>" % fnum(x) for x in r[1:])))
        a("</table>")
    if "memmp" in mods:
        mp_table(a, mods["memmp"], "memory &mdash; MP scaling")
    if "disk" in mods:
        d = mods["disk"]["data"]
        a("<h3>disk</h3>")
        if d.get("method"):
            a('<p class="small">%s</p>' % esc(d["method"]))
        a("<table><tr><th class='l'>test</th><th>MB/s</th><th>IOPS</th>"
          "<th>lat(ms)</th></tr>")
        for t in ("seq write", "seq read", "rand read", "rand write"):
            if t in d["tests"]:
                v = d["tests"][t]
                a("<tr><td class='l'>%s</td><td>%s</td><td>%s</td>"
                  "<td>%s</td></tr>" % (esc(t), fnum(v[0], 2),
                                        fnum(v[1]), fnum(v[2], 2)))
        a("</table>")
        for p in d.get("pctl", []):
            a('<p class="small">%s</p>' % esc(p))
    if "net" in mods:
        n = mods["net"]["data"]
        a("<h3>network</h3>")
        t = n["tcp"]
        bits = []
        if "rtt" in t:
            bits.append("%srtt %.2f ms" % ("UDP " if t.get("rtt_proto") == "UDP" else "", t["rtt"]))
        if "upload" in t:
            bits.append("TCP up %.2f / down %.2f Mbit/s" %
                        (t.get("upload", 0), t.get("download", 0)))
        if n.get("udpup"):
            u = n["udpup"][-1]
            bits.append("UDP up %.2f Mbit/s @ %dB (%.1f%% loss)" %
                        (u[1], int(u[0]), u[2] if len(u) > 2 else 0))
        a('<p class="small">%s &#8212; server %s</p>' %
          (esc("; ".join(bits)), esc(n.get("server", "?"))))
    return "".join(out)


def build_page(machines):
    hosts = list(machines)
    # color + order by VAX-MIPS descending
    rows = []
    for h in hosts:
        rows.append((h,) + summary_row(h, machines[h], "#000"))
    rows.sort(key=lambda r: -r[1])
    order = [r[0] for r in rows]
    color = {h: PALETTE[i % len(PALETTE)] for i, h in enumerate(order)}

    out = []
    a = out.append
    a("<!DOCTYPE html><html><head><meta charset='utf-8'>")
    a("<meta name='viewport' content='width=device-width, initial-scale=1'>")
    a("<title>sunbench &mdash; fleet</title><style>%s</style></head><body>" % CSS)
    a("<div class='wrap'>")
    a("<h1>sunbench <span class='v'>fleet</span></h1>")
    a("<p class='sub'>%d machines &middot; latest run per module &middot; "
      "generated %s</p>" % (len(hosts),
                            datetime.now().strftime("%Y-%m-%d %H:%M")))

    a("<div class='card'><h2>Summary</h2>")
    a("<p class='meta'>sorted by Dhrystone VAX-MIPS</p>")
    a("<table><tr><th class='l'>machine</th><th class='l'>model</th>"
      "<th>MHz</th><th class='l'>OS</th><th class='l'>RAM</th>"
      "<th>VAX-MIPS</th><th>MWIPS</th><th>daxpy<br>MFLOPS</th>"
      "<th>DRAM rd<br>MB/s</th><th>DRAM lat<br>ns</th>"
      "<th>disk seqR<br>MB/s</th><th>TCP up/dn<br>Mbit/s</th></tr>")
    for h in order:
        a(summary_row(h, machines[h], color[h])[1])
    a("</table></div>")

    def series_for(module, xcol, ycol, xfmt):
        xlabs, seen = [], set()
        per = []
        for h in order:
            sec = machines[h]["modules"].get(module)
            if not sec:
                continue
            d = {}
            for r in sec["data"]["rows"]:
                xl = xfmt(r[xcol])
                d[xl] = r[ycol]
                if xl not in seen:
                    seen.add(xl)
                    xlabs.append((r[xcol], xl))
            per.append((h, color[h], d))
        xlabs.sort(key=lambda t: t[0])
        return [xl for _, xl in xlabs], per

    a("<div class='card'><h2>Fleet charts</h2>")
    a("<p class='meta'>one color per machine, log scales</p>")
    xl, s = series_for("mem", 0, 1, sizelab)
    a(svg_chart("Memory read bandwidth vs working set", "MB/s, log",
                "working set", xl, s))
    xl, s = series_for("mem", 0, 5, sizelab)
    a(svg_chart("Memory load latency vs working set", "ns, log",
                "working set", xl, s))
    xl, s = series_for("disksweep", 0, 2, sizelab)
    a(svg_chart("Disk sequential read vs block size", "MB/s, log",
                "block size", xl, s))
    xl, s = series_for("disksweep", 0, 5, sizelab)
    a(svg_chart("Disk random-read IOPS vs block size", "IOPS, log",
                "block size", xl, s))

    def net_series(key):
        xlabs, seen, per = [], set(), []
        for h in order:
            sec = machines[h]["modules"].get("net")
            if not sec or not sec["data"].get(key):
                continue
            d = {}
            for r in sec["data"][key]:
                xl = sizelab(r[0] / 1024.0) if r[0] >= 1024 else "%dB" % int(r[0])
                d[xl] = r[1]
                if xl not in seen:
                    seen.add(xl)
                    xlabs.append((r[0], xl))
            per.append((h, color[h], d))
        xlabs.sort(key=lambda t: t[0])
        return [x for _, x in xlabs], per

    xl, s = net_series("wsweep")
    a(svg_chart("TCP upload vs write() size", "Mbit/s, log", "write size",
                xl, s))
    xl, s = net_series("udpup")
    a(svg_chart("UDP upload vs datagram size", "Mbit/s, log",
                "datagram size", xl, s))
    a("</div>")

    for h in order:
        m = machines[h]
        syn = m["syn"]
        a("<div class='card' id='%s'>" % esc(h))
        a("<h2><i class='dot' style='background:%s;width:11px;height:11px'>"
          "</i>%s</h2>" % (color[h], esc(h)))
        a("<p class='meta'>sunbench <b>%s</b> &middot; latest run %s &middot; "
          "%d run section%s parsed</p>" %
          (esc(m.get("version", "?")), esc(m.get("datestr", "?")),
           m["nruns"], "" if m["nruns"] == 1 else "s"))
        a("<div class='grid'>")
        for k in SYN_KEYS:
            if syn.get(k):
                a("<div class='row'><div class='k'>%s</div>"
                  "<div class='val'>%s</div></div>" % (esc(k), esc(syn[k])))
        a("</div>")
        a(module_tables(m))
        a("</div>")

    a("<p class='foot'>sunbench-fleet.py &mdash; regenerate with "
      "<code>./sunbench-fleet.py logs/ -o fleet.html</code></p>")
    a("</div></body></html>")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description="combine sunbench logs into "
                                             "one fleet HTML page")
    ap.add_argument("paths", nargs="+", help=".log files and/or directories")
    ap.add_argument("-o", "--out", default="fleet.html")
    args = ap.parse_args()
    machines = load(args.paths)
    if not machines:
        print("no sunbench log sections found", file=sys.stderr)
        return 1
    page = build_page(machines)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(page)
    print("%s: %d machines (%s)" % (args.out, len(machines),
                                    ", ".join(sorted(machines))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
