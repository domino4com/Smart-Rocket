import serial,time,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.2)
f=pathlib.Path('test-results/status-check.log').open('w')
def read_for(sec):
 end=time.monotonic()+sec
 while time.monotonic()<end:
  b=s.readline()
  if b: t=b.decode(errors='replace');f.write(t);f.flush();print(t.rstrip(),flush=True)
s.write(b'?r');read_for(3);s.write(b't');read_for(2);s.write(b's');read_for(3);s.write(b'b')
started=False;end=time.monotonic()+10;lines=[]
while time.monotonic()<end:
 line=s.readline().decode(errors='replace').strip()
 if line=='TXT_BEGIN':started=True;continue
 if line=='TXT_END':break
 if started:lines.append(line)
s.close();f.close()
text='\n'.join(lines)+'\n';pathlib.Path('test-results/status_readback.txt').write_text(text)
v=dict(x.split('=',1) for x in lines if '=' in x)
assert v['state']=='1',v
assert v['charging']=='1',v
assert int(v['battery_snapshot_age_ms'])>=2000,v
assert int(v['dropped'])==0,v
print('STATUS PASS: flight report uses cached battery; charging active; no dropped rows')
print(text)
