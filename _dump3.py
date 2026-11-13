p='src/core.cpp'
lines=open(p,encoding='utf-8',newline='').read().replace('\r\n','\n').split('\n')
for i in range(655, 700):
    print('%4d %s' % (i+1, lines[i]))
print('...')
for i in range(780, 815):
    print('%4d %s' % (i+1, lines[i]))
