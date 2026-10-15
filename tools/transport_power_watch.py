"""Read-only monitor that survives USB removal during transport-power tests."""
import pathlib, time
import serial

port = '/dev/cu.usbmodem11401'
start = time.monotonic()
connection = None
with pathlib.Path('test-results/transport-power-check.log').open('a') as log:
    def emit(message):
        text = f'[{time.monotonic()-start:.1f}s] {message}'
        print(text, flush=True)
        log.write(text+'\n')
        log.flush()
    try:
        deadline = start + 360
        next_query = 0
        while time.monotonic() < deadline:
            try:
                if connection is None:
                    connection = serial.Serial(port, 115200, timeout=.2)
                    emit('USB serial connected')
                    next_query = 0
                if time.monotonic() >= next_query:
                    connection.write(b'?')
                    next_query = time.monotonic() + 2
                line = connection.readline().decode(errors='replace').strip()
                if line:
                    emit(line)
            except (serial.SerialException, OSError):
                if connection is not None:
                    emit('USB serial disconnected; waiting for reconnect')
                    connection.close()
                    connection = None
                time.sleep(.3)
    finally:
        if connection is not None:
            connection.close()
