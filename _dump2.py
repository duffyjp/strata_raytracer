def dump(path, a, b):
    with open(path, 'r', encoding='utf-8', newline='') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')
    for i in range(a-1, min(b, len(lines))):
        print('%4d %s' % (i+1, lines[i]))
dump('src/core.cpp', 793, 830)
