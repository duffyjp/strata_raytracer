def dump(path, a, b):
    with open(path, 'r', encoding='utf-8', newline='') as f:
        lines = f.read().split('\n')
    print('===== %s lines %d..%d =====' % (path, a, b))
    for i in range(a-1, min(b, len(lines))):
        print('%4d %r' % (i+1, lines[i]))
dump('src/core.cpp', 39, 56)
dump('src/core.cpp', 356, 360)
dump('src/core.cpp', 378, 384)
dump('src/core.cpp', 426, 448)
