import json
d = json.load(open('tools/image-score.json', 'r', encoding='utf-8'))
n = sum(len(s['keys']) for p in d['pages'] for s in p['steps'])
print(f'total_steps={d["total_steps"]}, total_key_events={n}')
print(f'pages={len(d["pages"])}')

# Compare with angel-score.json
a = json.load(open('tools/angel-score.json', 'r', encoding='utf-8-sig'))
notes = a['data']['score']['tracks'][0]['notes']
print(f'angel-score notes count={len(notes)}')

# First 20 notes from angel-score
print('angel first 20 keyIndex:', [n['keyIndex'] for n in notes[:20]])

# First 20 non-empty steps from image-score
img_steps = [s for p in d['pages'] for s in p['steps'] if s['keys']]
print('image first 20 non-empty:', [s['keys'][0] if len(s['keys'])==1 else s['keys'] for s in img_steps[:20]])
