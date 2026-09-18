const fs = require('fs');
const data = JSON.parse(fs.readFileSync('angel-score.json', 'utf8'));
console.log('TOP KEYS:', Object.keys(data));
console.log('format:', data.format, 'bpm:', data.bpm, 'durationMs:', data.durationMs, 'noteCount:', data.noteCount);
console.log('timing:', JSON.stringify(data.timing));
console.log('tracks count:', Array.isArray(data.tracks) ? data.tracks.length : 'n/a');
const t0 = data.tracks && data.tracks[0];
if (t0) {
  console.log('track0 keys:', Object.keys(t0));
  console.log('track0 instrument:', t0.instrument, 'transposeSemitones:', t0.transposeSemitones);
  console.log('track0 notes len:', t0.notes ? t0.notes.length : 'n/a');
  const n0 = t0.notes && t0.notes[0];
  if (n0) {
    console.log('note0 fields:', Object.keys(n0));
    console.log('note0 sample:', JSON.stringify(n0));
  }
  // keyIndex distribution
  const dist = {};
  let badIdx = 0;
  for (const n of t0.notes) {
    const k = n.keyIndex;
    dist[k] = (dist[k] || 0) + 1;
    if (k < 1 || k > 15) badIdx++;
  }
  console.log('keyIndex distribution:', JSON.stringify(dist));
  console.log('bad keyIndex count (<1 or >15):', badIdx);
  // time field check
  const sampleTimes = t0.notes.slice(0, 5).map(n => n.startMs);
  console.log('sample startMs:', JSON.stringify(sampleTimes));
}
