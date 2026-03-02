# =========================
# VIBE BOT DISPLAY DEVICE: ESP-NOW -> OLED face + WS2812 + X27 stepper + button
# MicroPython on ESP32C3
# =========================

import time
from machine import Pin, SoftI2C
import network

try:
    import espnow
except ImportError:
    raise RuntimeError("This script requires MicroPython build with espnow support.")

# OLED driver (common MicroPython SSD1306 module)
import ssd1306

# WS2812 (NeoPixel)
import neopixel

# ---------- CONFIG: PINS (CHANGE THESE) ----------
# I2C pins for SSD1306
I2C_SCL = 5
I2C_SDA = 4

# WS2812
NEOPIXEL_PIN = 2
NEOPIXEL_COUNT = 1  # set to however many you actually have

# Stepper coil pins (4-wire)
STEP_A = 6
STEP_B = 7
STEP_C = 8
STEP_D = 9

# Button pin (active low to GND)
BUTTON_PIN = 10

# OLED size
OLED_W = 128
OLED_H = 64

# BPM range
MIN_BPM = 60
MAX_BPM = 230

# ---------- ESP-NOW ----------
WIFI_CHANNEL = 1

def setup_espnow():
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    sta.disconnect()
    e = espnow.ESPNow()
    e.active(True)
    return e

# ---------- Face rendering ----------
def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x

def bpm_to_mood(bpm):
    """
    0.0 = relaxed, 1.0 = angry
    """
    bpm = clamp(bpm, MIN_BPM, MAX_BPM)
    return (bpm - MIN_BPM) / (MAX_BPM - MIN_BPM)

def draw_face(oled, bpm):
    oled.fill(0)

    mood = bpm_to_mood(bpm)

    # Face frame
    oled.rect(0, 0, OLED_W, OLED_H, 1)

    # Eyes position
    eye_y = 18
    left_x = 35
    right_x = 80

    # Eyebrows: relaxed = flatter, angry = slanted down towards center
    # We'll draw eyebrows with lines
    # left brow
    lb_y1 = int(eye_y - 10 - 4 * mood)
    lb_y2 = int(eye_y - 10 + 6 * mood)
    oled.line(left_x - 10, lb_y1, left_x + 10, lb_y2, 1)

    # right brow
    rb_y1 = int(eye_y - 10 + 6 * mood)
    rb_y2 = int(eye_y - 10 - 4 * mood)
    oled.line(right_x - 10, rb_y1, right_x + 10, rb_y2, 1)

    # Eyes: relaxed = smaller/rounder, angry = more squint
    # Draw as rectangles with height varying
    eye_h = int(6 - 3 * mood)  # 6 -> 3
    eye_h = clamp(eye_h, 2, 6)
    oled.fill_rect(left_x - 6, eye_y, 12, eye_h, 1)
    oled.fill_rect(right_x - 6, eye_y, 12, eye_h, 1)

    # Mouth: relaxed = slight smile, angry = frown/open
    mouth_y = 44
    mouth_w = 40
    mouth_x = (OLED_W - mouth_w) // 2

    if mood < 0.33:
        # small smile
        oled.line(mouth_x, mouth_y, mouth_x + mouth_w, mouth_y, 1)
        oled.line(mouth_x + 5, mouth_y + 3, mouth_x + mouth_w - 5, mouth_y + 3, 1)
    elif mood < 0.66:
        # neutral / straight
        oled.line(mouth_x, mouth_y + 2, mouth_x + mouth_w, mouth_y + 2, 1)
    else:
        # angry/frown
        oled.line(mouth_x, mouth_y + 4, mouth_x + mouth_w, mouth_y + 4, 1)
        oled.line(mouth_x + 5, mouth_y + 1, mouth_x + mouth_w - 5, mouth_y + 1, 1)

    # BPM text
    oled.text("BPM {}".format(int(bpm)), 4, 4, 1)
    oled.show()

# ---------- WS2812 behavior ----------
def set_led(np, bpm, t_ms):
    """
    Under 100 BPM: steady mild (dim).
    Over 100 BPM: party flash based on time and bpm.
    """
    if bpm is None:
        # off
        for i in range(np.n):
            np[i] = (0, 0, 0)
        np.write()
        return

    if bpm <= 100:
        # calm status glow (dim white-ish)
        for i in range(np.n):
            np[i] = (10, 10, 10)
        np.write()
        return

    # party: flash speed scales with bpm
    # period goes from ~600ms at 100bpm down to ~200ms at 230bpm
    period = int(600 - 400 * ((clamp(bpm, 100, MAX_BPM) - 100) / (MAX_BPM - 100)))
    phase = (t_ms // max(1, period)) % 6

    colors = [
        (50, 0, 0),
        (0, 50, 0),
        (0, 0, 50),
        (50, 50, 0),
        (0, 50, 50),
        (50, 0, 50),
    ]
    c = colors[phase]
    for i in range(np.n):
        np[i] = c
    np.write()

# ---------- X27 Stepper control (simple 4-coil sequence) ----------
# Half-step sequence (8 steps) – works for many 4-coil steppers
SEQ = [
    (1,0,0,0),
    (1,1,0,0),
    (0,1,0,0),
    (0,1,1,0),
    (0,0,1,0),
    (0,0,1,1),
    (0,0,0,1),
    (1,0,0,1),
]

class Stepper4:
    def __init__(self, a, b, c, d, step_delay_ms=2):
        self.pins = [Pin(a, Pin.OUT), Pin(b, Pin.OUT), Pin(c, Pin.OUT), Pin(d, Pin.OUT)]
        self.idx = 0
        self.pos = 0  # step position
        self.step_delay_ms = step_delay_ms
        self.off()

    def off(self):
        for p in self.pins:
            p.value(0)

    def _apply(self, s):
        for p, v in zip(self.pins, s):
            p.value(v)

    def step(self, direction=1):
        self.idx = (self.idx + direction) % len(SEQ)
        self._apply(SEQ[self.idx])
        self.pos += direction
        time.sleep_ms(self.step_delay_ms)

    def move_to(self, target_steps, max_steps_per_call=20):
        # Move gradually to avoid blocking too long
        delta = target_steps - self.pos
        if delta == 0:
            return
        direction = 1 if delta > 0 else -1
        steps = min(abs(delta), max_steps_per_call)
        for _ in range(steps):
            self.step(direction)

def bpm_to_step_target(bpm):
    """
    Map BPM to step position for a gauge/needle.
    Tune these numbers to match your motor + desired sweep.
    """
    bpm = clamp(bpm, MIN_BPM, MAX_BPM)
    # Example: 0..600 steps sweep across the range
    sweep_steps = 600
    return int((bpm - MIN_BPM) * sweep_steps / (MAX_BPM - MIN_BPM))

# ---------- MAIN ----------
def run():
    # Button
    btn = Pin(BUTTON_PIN, Pin.IN, Pin.PULL_UP)

    # OLED
    i2c = SoftI2C(scl=Pin(I2C_SCL), sda=Pin(I2C_SDA), freq=400000)
    oled = ssd1306.SSD1306_I2C(OLED_W, OLED_H, i2c)

    # NeoPixel
    np = neopixel.NeoPixel(Pin(NEOPIXEL_PIN), NEOPIXEL_COUNT)

    # Stepper
    stepper = Stepper4(STEP_A, STEP_B, STEP_C, STEP_D, step_delay_ms=2)

    # ESP-NOW
    e = setup_espnow()

    bpm = None
    system_on = True
    last_draw = time.ticks_ms()

    def shutdown():
        nonlocal system_on
        system_on = False
        oled.fill(0)
        oled.text("OFF", 54, 28, 1)
        oled.show()
        for i in range(np.n):
            np[i] = (0, 0, 0)
        np.write()
        stepper.off()

    # Initial screen
    oled.fill(0)
    oled.text("Vibe Bot", 40, 24, 1)
    oled.text("Waiting...", 28, 40, 1)
    oled.show()

    while True:
        # button to shut off
        if system_on and btn.value() == 0:
            # simple debounce
            time.sleep_ms(30)
            if btn.value() == 0:
                shutdown()

        if not system_on:
            time.sleep_ms(50)
            continue

        # Receive BPM messages
        host, msg = e.recv(0)  # non-blocking
        if msg:
            try:
                s = msg.decode() if isinstance(msg, (bytes, bytearray)) else str(msg)
                if s.startswith("BPM:"):
                    bpm = int(s.split(":")[1])
                    bpm = clamp(bpm, MIN_BPM, MAX_BPM)
            except Exception:
                pass

        now = time.ticks_ms()

        # Update LED frequently
        set_led(np, bpm, now)

        # Update face + motor at a reasonable rate
        if bpm is not None and time.ticks_diff(now, last_draw) > 120:
            draw_face(oled, bpm)

            # Move stepper towards target (smooth)
            target = bpm_to_step_target(bpm)
            stepper.move_to(target, max_steps_per_call=15)

            last_draw = now

        time.sleep_ms(5)

run()