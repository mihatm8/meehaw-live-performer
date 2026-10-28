#!/usr/bin/env python3
# Python model of the smfParse rules in firmware/liveplayer/src/gen/smf.cpp, used to derive the corpus
# constants in tests/test_key_smf.cpp. It is a port of the same rules (FIFO note matching per pitch and
# channel, 16-beat window with early exit, kMaxNotes eviction, trailing-silence length rule), so the numbers
# it produces are a regression snapshot of those rules rather than an independent oracle.
# Usage: python3 -I tests/refmodel_smf.py sdcard/lp/midi
import os, sys, struct, collections
PPQN = 96; MAXT = 1536; UNIT = 12; MAXN = 192; ACTIVE = 64
def vlq(b, i):
    v = 0
    for _ in range(8):
        c = b[i]; i += 1; v = ((v << 7) | (c & 0x7F)) & 0xFFFFFFFF
        if c < 0x80:
            if v >= 0x80000000: v -= 0x100000000
            return v, i
    raise EOFError
CAP = 1 << 30
def clampT(t): return max(-CAP, min(CAP, t))
def to96(t, ppq): return 0 if t <= 0 else min(MAXT + 1, (t * PPQN + ppq // 2) // ppq)
def ceil_beats(t): return ((t + PPQN - 1) // PPQN) * PPQN
def parse(b):
    if b[:4] != b'MThd': return None
    hl = struct.unpack('>I', b[4:8])[0]; fmt, ntr, div = struct.unpack('>HHH', b[8:14]); i = 8 + hl
    if fmt > 1 or div == 0 or div & 0x8000: return None
    ppq = div; limitSrc = -(-((MAXT + 1) * ppq - ppq // 2) // PPQN)      # smallest src tick with to96 > MAXT
    notes = []; bpm = 0.0; eotAll = 0; st = dict(maxEndSrc=0, maxEnd=0, cut=False, beyond=False); tracks = 0
    def emit(s, e, p, v):
        st['maxEndSrc'] = max(st['maxEndSrc'], e)
        s96 = to96(s, ppq); e96 = to96(e, ppq)
        if e96 <= s96: e96 = s96 + 1
        st['maxEnd'] = max(st['maxEnd'], min(e96, MAXT))
        if s96 >= MAXT: st['cut'] = True; return
        if e96 > MAXT: e96 = MAXT; st['cut'] = True
        lu = min(255, max(1, -(-(e96 - s96) // UNIT)))
        n = (s96, p, lu, v >> 2)
        if len(notes) < MAXN: notes.append(n); return
        st['cut'] = True
        li = max(range(MAXN), key=lambda k: (notes[k][0], k))
        if s96 < notes[li][0]: notes[li] = n
    while i + 8 <= len(b) and tracks < ntr:
        cid = b[i:i+4]; tl = struct.unpack('>I', b[i+4:i+8])[0]; i += 8; end = min(i + tl, len(b))
        if cid != b'MTrk': i = end; continue
        tracks += 1; tm = 0; status = 0; active = []
        while i < end:
          try:
              d, i = vlq(b, i); tm = clampT(tm + d)
              if tm >= limitSrc: st['beyond'] = True; eotAll = max(eotAll, tm); break
              s = b[i]
              if s < 0x80:
                  if status == 0: break
                  s = status
              else: i += 1
              if s == 0xFF:
                  ty = b[i]; i += 1; l, i = vlq(b, i); data = b[i:i+l]; i += l
                  if ty == 0x2F: eotAll = max(eotAll, tm); break
                  if ty == 0x51 and l == 3 and bpm == 0 and int.from_bytes(data, 'big'): bpm = 60e6 / int.from_bytes(data, 'big')
              elif s in (0xF0, 0xF7): l, i = vlq(b, i); i += l
              elif s >= 0xF0: i += {0xF1: 1, 0xF2: 2, 0xF3: 1}.get(s, 0)
              else:
                  status = s; kind = s & 0xF0; ch = s & 0x0F
                  d1 = b[i]; i += 1
                  if d1 >= 0x80: break
                  if kind in (0xC0, 0xD0): continue
                  d2 = b[i]; i += 1
                  if d2 >= 0x80: break
                  if kind == 0x90 and d2 > 0:
                      if len(active) >= ACTIVE:
                          st['cut'] = True; p0, c0, s0, v0 = active.pop(0); emit(s0, tm, p0, v0)
                      active.append((d1, ch, tm, d2))
                  elif kind in (0x80, 0x90):
                      idx = next((j for j, a in enumerate(active) if a[0] == d1 and a[1] == ch), None)
                      if idx is None: idx = next((j for j, a in enumerate(active) if a[0] == d1), None)
                      if idx is not None:
                          p0, c0, s0, v0 = active.pop(idx); emit(s0, tm, p0, v0)
          except (IndexError, EOFError): break
        for p0, c0, s0, v0 in active: emit(s0, tm, p0, v0)
        i = end
    if tracks == 0: return None
    if st['beyond']: L = MAXT; st['cut'] = True
    else:
        eot96 = to96(eotAll, ppq); maxEnd = st['maxEnd']
        L = ceil_beats(eot96) if (eot96 > maxEnd and eot96 - maxEnd <= 2 * PPQN) else ceil_beats(maxEnd)
        L = max(PPQN, L)
    out = []
    for (s, p, lu, v) in notes:
        if s + lu * UNIT > L:
            avail = L - s
            lu = avail // UNIT if avail >= UNIT else 1
        out.append((s, p, lu, v))
    out.sort()
    return dict(L=L, notes=out, bpm=bpm, trunc=st['cut'], srcLen=max(0, st['maxEndSrc'], eotAll), ppq=ppq)
if __name__ == '__main__':
    root = sys.argv[1]; agg = collections.OrderedDict(); hist = collections.Counter(); exact = {}
    want_exact = ['Beat/NA/NA__Beat__DPH-K4-GROOVE-C-3__e57c6fb1.mid', 'Chord/Amin/Amin__Chord__DHS-20-UptempoPiano-Am__cb4c2447.mid',
                  'Bass/Dmin/Dmin__Chord__BassDerived__Bass-DM-Analog-Dm-5PN__7873ea13.mid', 'Bass/A/Amaj__Bass__1-2-3-4-Bass-A__052c0ae1.mid',
                  'Pad/Emin/Emin__Pad__Pad-DM-SineSpace-1-Em-5PN__ac753c56.mid', 'Bass/A#min/A#min__Bass__Shift-Along-Bass-1-Bb-min__a119ad34.mid',
                  'Bass/G#min/G#min__Bass__DBS-Ascending1-G#m__822cf421.mid', 'Percussion/NA/NA__Percussion__MT-HH-LP13__c5b2539f.mid',
                  'Beat/NA/NA__Beat__DHB-KI-06-GROOVE-E__a18a445a.mid', 'Bass/Dmin/Dmin__Bass__Bass-DM-Foundation-Dm-5PN__2c115ed6.mid',
                  'Bass/Fmin/Fmin__Bass__Bookit-Bass-F-min__9a4f9a6c.mid', 'Bass/A#min/A#min__Bass__DHB-123BPM-PART-09-BASS-01-A#__b5cfb398.mid']
    tail = 0
    for cat in sorted(os.listdir(root)):
        cdir = os.path.join(root, cat)
        if not os.path.isdir(cdir): continue
        a = agg.setdefault(cat, collections.Counter())
        for key in sorted(os.listdir(cdir)):
            kdir = os.path.join(cdir, key)
            for f in sorted(os.listdir(kdir)):
                rel = '%s/%s/%s' % (cat, key, f); r = parse(open(os.path.join(kdir, f), 'rb').read())
                if r is None: a['fail'] += 1; continue
                a['files'] += 1; a['notes'] += len(r['notes']); a['trunc'] += r['trunc']; a['beats'] += r['L'] // 96
                a['tempo'] += (r['bpm'] > 0)
                hist['%s:%d' % (cat, r['L'] // 96)] += 1
                if rel in want_exact: exact[rel] = r
                for (s, p, lu, v) in r['notes']:
                    assert s < r['L'] and s + lu * UNIT < r['L'] + UNIT, (rel, s, lu, r['L'])
                    if s + lu * UNIT > r['L']: tail += 1
                assert 96 <= r['L'] <= 1536 and r['L'] % 96 == 0
                if cat == 'Beat':
                    for (s, p, lu, v) in r['notes']: assert 36 <= p <= 58, (rel, p)
    tot = collections.Counter()
    for cat, a in agg.items():
        print(cat, dict(a)); tot.update(a)
    print('TOTAL', dict(tot), 'notes overrunning the loop end by < 12 ticks:', tail)
    print('--- hist')
    for k in sorted(hist, key=lambda s: (s.split(':')[0], int(s.split(':')[1]))): print(k, hist[k])
    print('--- exact')
    for rel in want_exact:
        if rel not in exact: continue
        r = exact[rel]
        print(rel, 'L', r['L'], 'n', len(r['notes']), 'bpm %.4f' % r['bpm'], 'trunc', r['trunc'], 'srcLen', r['srcLen'], 'ppq', r['ppq'])
        print('   first', r['notes'][:6]); print('   last', r['notes'][-3:])
