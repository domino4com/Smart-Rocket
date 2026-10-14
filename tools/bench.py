import serial,time,pathlib
p=pathlib.Path('test-results');p.mkdir(exist_ok=True)
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.1)
f=(p/'bench.log').open('w')
def read_for(sec):
 end=time.monotonic()+sec
 while time.monotonic()<end:
  b=s.readline()
  if b:
   t=b.decode(errors='replace'); f.write(t);f.flush();print(t.rstrip(),flush=True)
s.write(b'?v');read_for(3)
s.write(b'tr');read_for(65)
s.write(b'?');read_for(1) # confirm still recording after more than one minute
s.write(b's');read_for(3)
s.write(b'?l');read_for(3)
s.close();f.close()
