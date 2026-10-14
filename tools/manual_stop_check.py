import serial,time,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.1)
p=pathlib.Path('test-results/manual-stop-bench.log');f=p.open('w')
def capture(seconds):
 end=time.monotonic()+seconds;lines=[];next_query=0
 while time.monotonic()<end:
  if time.monotonic()>=next_query:s.write(b'?');next_query=time.monotonic()+5
  b=s.readline()
  if b:
   t=b.decode(errors='replace');lines.append(t);print(t.rstrip(),flush=True);f.write(t);f.flush()
 return lines
try:
 s.write(b'r');capture(66)
 s.write(b'?');lines=capture(2)
 assert any('STATUS state=1 ' in t for t in lines),'Recording did not remain active beyond one minute'
 print('PASS: STILL RECORDING AFTER 66 SECONDS',flush=True)
 # Watch for the human Play press. Leave recording running while waiting.
 lines=capture(120)
 print('BUTTON_CLOSED='+str(any('CLOSED rows=' in t for t in lines)),flush=True)
finally:s.close();f.close()
