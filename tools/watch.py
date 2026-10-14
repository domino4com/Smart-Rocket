import serial,time,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.2)
f=pathlib.Path('test-results/physical-check.log').open('a')
try:
 s.write(b'?');end=time.monotonic()+100
 while time.monotonic()<end:
  b=s.readline()
  if b:
   t=b.decode(errors='replace');print(t.rstrip(),flush=True);f.write(t);f.flush()
finally:s.close();f.close()
