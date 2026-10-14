import serial,time,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.1)
f=pathlib.Path('test-results/arming-check.log').open('a')
try:
 end=time.monotonic()+180;next_query=0
 while time.monotonic()<end:
  if time.monotonic()>=next_query:s.write(b'?');next_query=time.monotonic()+1
  b=s.readline()
  if b:
   t=b.decode(errors='replace');print(t.rstrip(),flush=True);f.write(t);f.flush()
finally:s.close();f.close()
