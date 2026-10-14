import serial,time,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.2)
f=pathlib.Path('test-results/power-key-check.log').open('a')
try:
 end=time.monotonic()+120;nextq=0
 while time.monotonic()<end:
  if time.monotonic()>nextq:s.write(b'?');nextq=time.monotonic()+2
  b=s.readline()
  if b:t=b.decode(errors='replace');print(t.rstrip(),flush=True);f.write(t);f.flush()
finally:s.close();f.close()
