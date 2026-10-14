import serial,time,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.2)
s.write(b'r');time.sleep(1);s.close()
time.sleep(8)
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.2);s.write(b'?s')
end=time.monotonic()+4;lines=[]
while time.monotonic()<end:
 b=s.readline()
 if b:
  t=b.decode(errors='replace').rstrip();lines.append(t);print(t,flush=True)
s.close();pathlib.Path('test-results/no-host-check.log').write_text('\n'.join(lines)+'\n')
assert any('CLOSED' in x for x in lines),'Did not close'
