import serial,time,csv,json,statistics,math,pathlib
s=serial.Serial('/dev/cu.usbmodem11401',115200,timeout=.5)
s.reset_input_buffer()
# Confirm application startup/USB connection before requesting a transfer.
# Opening a port immediately after upload can precede setup() and lose 'd'.
ready=False;until=time.monotonic()+15;next_query=0
with pathlib.Path('test-results/readback-handshake.log').open('w') as debug:
 while time.monotonic()<until:
  if time.monotonic()>=next_query:s.write(b'?');next_query=time.monotonic()+2
  line=s.readline().decode(errors='replace').strip()
  if line:debug.write(line+'\n');debug.flush()
  if line.startswith('STATUS state='):
   assert 'state=1 ' not in line and 'state=2 ' not in line, 'Close recording before downloading'
   assert 'state=4 ' not in line, 'Device reports a fault'
   ready=True;break
assert ready, 'Device did not respond to startup handshake (see readback-handshake.log)'
s.write(b'd')
rows=[]; header=None; started=False; ended=False; deadline=time.monotonic()+90
p=pathlib.Path('test-results/flight_readback.csv')
with p.open('w') as f:
 while time.monotonic()<deadline:
  line=s.readline().decode(errors='replace').strip()
  if line=='CSV_BEGIN':started=True;continue
  if line=='CSV_END':ended=True;break
  if not started:continue
  if line.startswith('millis,'):header=line;f.write(line+'\n');continue
  if line and line[0].isdigit() and len(line.split(','))==12: f.write(line+'\n');rows.append(line.split(','))
s.close()
assert started and ended and header and rows, 'Incomplete dump'
seq=[int(r[3]) for r in rows];us=[int(r[2]) for r in rows]
elapsed=[int(r[1]) for r in rows]
assert elapsed[0]<1000, 'First elapsed timestamp wrapped or started late'
assert all(b>=a for a,b in zip(elapsed,elapsed[1:])), 'Elapsed timestamp went backwards'
assert all(b==a+1 for a,b in zip(seq,seq[1:])), 'Sequence gaps'
assert all(int(r[11])==0 for r in rows),'Queue losses'
assert all(all(math.isfinite(float(r[j])) for j in range(4,9)) for r in rows),'Invalid sensor values'
assert all(b>a for a,b in zip(us,us[1:])),'Nonmonotonic timestamps'
assert seq[0]==0,'Lost beginning'
result={'rows':len(rows),'duration_ms':int(rows[-1][1])-int(rows[0][1]),'rate_hz':(len(rows)-1)*1e6/(us[-1]-us[0]),'max_gap_us':max(b-a for a,b in zip(us,us[1:])),'mean_xyz_g':[statistics.mean(float(r[j]) for r in rows) for j in range(4,7)],'mean_temp_C':statistics.mean(float(r[7]) for r in rows),'mean_pressure_Pa':statistics.mean(float(r[8]) for r in rows),'max_pressure_age_ms':max(int(r[9]) for r in rows),'queue_drops':0,'sequence_contiguous':True}
pathlib.Path('test-results/readback.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
