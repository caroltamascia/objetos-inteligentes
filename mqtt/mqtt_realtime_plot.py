#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque
from datetime import datetime

# ────────── Configuration ──────────
BROKER   = "localhost"
PORT     = 1883
TOPIC    = "graph/data"
MAX_LEN  = 2000
INTERVAL = 1000   # ms between updates

# ────────── Data buffers ──────────
times  = deque(maxlen=MAX_LEN)
values = deque(maxlen=MAX_LEN)
flags  = deque(maxlen=MAX_LEN)

# ────────── MQTT callbacks ──────────
def on_connect(client, userdata, flags, rc):
    print(f"[MQTT] Connected (rc={rc}), subscribing to {TOPIC}")
    client.subscribe(TOPIC)

def on_message(client, userdata, msg):
    try:
        t_str, val_str, flag_str = msg.payload.decode().split(",")
        t_fmt = datetime.strptime(t_str, "%Y-%m-%d %H:%M:%S").strftime("%H:%M:%S")
        times.append(t_fmt)
        values.append(int(val_str))
        flags.append(int(flag_str))
        # Debug print:
        print(f"[MQTT] {t_fmt} → {val_str}, flag={flag_str} (buffer size {len(times)})")
    except Exception as e:
        print(f"[MQTT] Bad payload: {e} – {msg.payload!r}")

# ────────── Set up MQTT client ──────────
client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT, 60)
client.loop_start()

# ────────── Set up Matplotlib figure ──────────
fig, ax = plt.subplots(figsize=(10,4))
plt.title("Real-Time Humidity (green = ON, red = OFF)")
plt.xlabel("Time")
plt.ylabel("Value")
line, = ax.plot([], [], lw=0.8, color="#888888")
scat = ax.scatter([], [], s=30)

def init():
    ax.clear()
    plt.xticks(rotation=45)
    plt.tight_layout()
    return line, scat

def update(frame):
    if not times:
        return line, scat

    x = list(range(len(times)))
    y = list(values)
    c = ["green" if f==1 else "red" for f in flags]

    ax.clear()
    ax.plot(x, y, color="#888888", linewidth=0.8, zorder=1)
    ax.scatter(x, y, color=c, s=30, zorder=2)
    ax.set_xticks(x[::max(1, len(x)//10)])
    ax.set_xticklabels([times[i] for i in x[::max(1, len(x)//10)]])
    ax.set_xlabel("Time")
    ax.set_ylabel("Humidity")
    ax.set_title("Real-Time Humidity (green = ON, red = OFF)")
    plt.tight_layout()
    return line, scat

ani = animation.FuncAnimation(
    fig, update, init_func=init,
    blit=False, interval=INTERVAL
)

# ────────── Run ──────────
plt.show()
