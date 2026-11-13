p='src/core.cpp'
lines=open(p,encoding='utf-8',newline='').read().replace('\r\n','\n').split('\n')
for i in range(700, 800):
    print('%4d %s' % (i+1, lines[i]))
