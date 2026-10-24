#!/usr/bin/env python3
"""Generate editable technical illustrations. Rasterize SVGs with any SVG renderer."""
from pathlib import Path
from html import escape

OUT = Path(__file__).resolve().parents[1] / "docs" / "images"
OUT.mkdir(parents=True, exist_ok=True)
INK, MUTED, CYAN, AMBER = "#edf4f7", "#93a9b6", "#56dbe0", "#ffc775"

def text(x, y, value, size=24, color=INK, weight=400, spacing=0):
    return f'<text x="{x}" y="{y}" fill="{color}" font-size="{size}" font-weight="{weight}" letter-spacing="{spacing}" font-family="Helvetica,Arial,sans-serif">{escape(value)}</text>'

def line(x1, y1, x2, y2, color=CYAN, width=3, dash=""):
    return f'<path d="M{x1} {y1}L{x2} {y2}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round" stroke-dasharray="{dash}"/>'

def rect(x, y, w, h, fill, radius=12, stroke="none", sw=2):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'

def circle(x, y, r, fill, stroke="none", sw=2):
    return f'<circle cx="{x}" cy="{y}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'

def svg(name, w, h, body):
    content = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
<title>Hue Light Control — {name} technical illustration</title>
<desc>Authored diagram of an ESP32, rotary encoder and SSD1306 OLED controller. Not a photograph.</desc>
<defs><radialGradient id="glow"><stop stop-color="#19424b"/><stop offset="1" stop-color="#0c151f"/></radialGradient><pattern id="grid" width="64" height="64" patternUnits="userSpaceOnUse"><path d="M64 0H0V64" fill="none" stroke="#203240" stroke-width="1"/></pattern></defs>
{rect(0,0,w,h,"#0c151f",0)}{circle(w*.75,h*.42,h*.64,"url(#glow)")}{rect(0,0,w,h,"url(#grid)",0)}
{body}</svg>'''
    (OUT / f"{name}.svg").write_text(content)

def board(x, y, s=1):
    pins = ''.join(rect(-10, 52+i*19, 24, 8, "#c5aa76", 2) + rect(196, 52+i*19, 24, 8, "#c5aa76", 2) for i in range(11))
    b = rect(0,0,210,300,"#143b39",18,"#40716c")+pins+rect(30,30,150,155,"#85959f",7,"#c0cbd0")
    b += rect(38,38,134,42,"#162331",4)+line(49,50,157,50,"#81939b",3)+line(49,63,143,63,"#81939b",3)
    b += text(52,125,"ESP32",29,"#172631",700)+text(53,153,"Wi-Fi / MCU",14,"#172631")
    b += rect(84,266,42,35,"#9daab0",5,"#d6e0e4")+circle(39,219,7,AMBER)+rect(80,217,50,24,"#0d2425",4)
    b += line(25,253,175,253,"#4a7871",2)
    return f'<g transform="translate({x} {y}) scale({s})">{b}</g>'

def oled(x, y, s=1):
    b = rect(0,0,340,190,"#182b39",15,"#466176")+rect(22,18,296,150,"#080e14",5,"#68808f",1)
    b += text(39,49,"HUE LIGHTS",20,AMBER,700)+text(271,49,"OK",18,AMBER)
    b += text(40,105,"56%",43,CYAN,700)+text(225,104,"ON",26,CYAN,700)
    b += rect(40,126,260,10,"#162e3e",5)+rect(40,126,146,10,CYAN,5)
    b += ''.join(circle(z,179,3,"#b7a474") for z in (139,159,179,199))
    return f'<g transform="translate({x} {y}) scale({s})">{b}</g>'

def knob(x, y, s=1):
    b = rect(-77,-77,154,154,"#203142",15,"#456174")+circle(0,0,64,"#142535",AMBER,3)+circle(0,0,49,"#263d4a","#81939d",2)
    b += circle(0,0,38,"#344c58")+line(0,-34,0,-56,AMBER,6)
    b += ''.join(rect(v,74,7,17,"#c5aa76",1) for v in (-40,-20,0,20,40))
    return f'<g transform="translate({x} {y}) scale({s})">{b}</g>'

def device(x, y, s=1):
    # Abstract component arrangement; actual pin connections appear in wiring.svg.
    b = line(138,280,138,130,CYAN,3)+line(236,325,393,325,AMBER,3)
    b += oled(10,0,.95)+board(45,230,.9)+knob(386,325,1.12)
    b += text(18,211,"SSD1306 · 128 × 64",20,MUTED)+text(61,537,"ESP32",21,MUTED)+text(300,464,"ROTARY ENCODER",19,MUTED)
    return f'<g transform="translate({x} {y}) scale({s})">{b}</g>'

def footer(w,h):
    return text(80,h-55,"ZACHARY STURMAN",19,MUTED,500,3)+text(w-385,h-55,"TECHNICAL ILLUSTRATION",17,MUTED,400,2)

def icon_symbol(x,y,s=1):
    b=circle(0,0,125,"#172a39",CYAN,6)+circle(0,0,95,"#213c4a","#61818b",3)+line(0,-75,0,-116,AMBER,12)+circle(0,0,55,"#304d59")
    b+=circle(0,0,23,AMBER)+''.join(line(a,b,c,d,AMBER,5) for a,b,c,d in [(0,-39,0,-48),(0,39,0,48),(-39,0,-48,0),(39,0,48,0),(-28,-28,-34,-34),(28,28,34,34),(-28,28,-34,34),(28,-28,34,-34)])
    return f'<g transform="translate({x} {y}) scale({s})">{b}</g>'

hero=text(95,126,"HARDWARE / EMBEDDED SYSTEMS",27,CYAN,500,4)
hero+=text(95,310,"Hue Light",119,INK,700)+text(95,444,"Control",119,INK,700)
hero+=text(101,550,"Everyday lighting.",47,INK)+text(101,615,"A physical control.",47,INK)
hero+=line(104,689,797,689,"#2c4655",2)
for i,label in enumerate(["TURN TO DIM", "TAP TO SWITCH", "HOLD FOR SETTINGS"]):
    hero+=text(104,770+i*68,label,27,MUTED,500,2)
hero+=device(1120,220,1.87)
hero+=rect(102,1053,462,68,"#152d39",34,"#375864",1)+text(137,1097,"ESP32  ·  OLED  ·  PHILIPS HUE",23,CYAN,500)
hero+=footer(2400,1350)
svg("hero",2400,1350,hero)

thumb=text(72,88,"HARDWARE / FIRMWARE",22,CYAN,500,3)+text(72,245,"Hue Light",81,INK,700)+text(72,339,"Control",81,INK,700)
thumb+=text(76,430,"Lighting at your fingertips.",27,MUTED)+device(773,167,1.4)
thumb+=text(76,615,"TURN",24,AMBER,700)+text(76,654,"Brightness",24,MUTED)+text(280,615,"TAP",24,AMBER,700)+text(280,654,"On / off",24,MUTED)
thumb+=line(76,746,1490,746,"#2c4655",2)+text(76,805,"ESP32 + ROTARY ENCODER + OLED",23,MUTED,400,2)+text(1115,805,"TECHNICAL ILLUSTRATION",14,MUTED,400,1)
svg("thumbnail",1600,900,thumb)

banner=text(88,98,"HARDWARE / EMBEDDED SYSTEMS",22,CYAN,500,3)+text(88,247,"Hue Light Control",96,INK,700)
banner+=text(92,329,"A physical control for everyday lighting.",35,MUTED)+text(94,470,"ESP32  /  ROTARY ENCODER  /  SSD1306 OLED",24,CYAN,400,2)
banner+=oled(1390,120,1.25)+knob(2075,244,1.6)+footer(2400,600)
svg("banner",2400,600,banner)

poster=text(80,111,"HARDWARE / FIRMWARE",25,CYAN,500,3)+text(80,254,"Hue Light",105,INK,700)+text(80,376,"Control",105,INK,700)
poster+=device(211,491,1.7)
poster+=line(84,1440,1116,1440,"#2c4655",2)+text(84,1495,"TURN · TAP · HOLD",30,AMBER,700)+text(84,1542,"TECHNICAL ILLUSTRATION",17,MUTED,400,3)
svg("poster",1200,1600,poster)
svg("icon",512,512,icon_symbol(256,256,1.45))

# Documented electrical connections, distinct from the abstract hero arrangement.
w=text(80,100,"Hue Light Control",62,INK,700)+text(84,157,"WIRING + LOCAL NETWORK ARCHITECTURE",23,CYAN,500,3)
w+=rect(80,230,510,620,"#142b37",22,"#3a5968")+text(116,291,"ESP32 / ESP32DEV",32,INK,700)
rows=[("GPIO 23","OLED SDA",CYAN), ("GPIO 18","OLED SCL",CYAN), ("GPIO 19","Encoder SW",AMBER), ("GPIO 21","Encoder CLK / A",AMBER), ("GPIO 22","Encoder DT / B",AMBER), ("3V3","OLED + encoder VCC","#a3e6b4"), ("GND","Common ground",MUTED)]
for i,(pin,label,col) in enumerate(rows):
    y=360+i*64
    w+=text(116,y,pin,27,col,700)+line(310,y-9,648,y-9,col,3)+circle(648,y-9,5,col)+text(670,y,label,27,INK)
w+=text(115,809,"INPUT PINS USE INTERNAL PULL-UPS",16,MUTED,400,1)
w+=rect(1120,237,380,276,"#142b37",22,"#3a5968")+text(1158,293,"SSD1306 OLED",29,INK,700)+oled(1153,329,.91)
w+=rect(1120,555,380,295,"#142b37",22,"#3a5968")+knob(1298,684,1.04)+text(1171,819,"PUSH-BUTTON ENCODER",20,INK,500)
w+=line(90,921,1500,921,"#2c4655",2)+text(84,985,"CONTROLLER",23,CYAN,500,2)+text(590,985,"HUE BRIDGE",23,CYAN,500,2)+text(1160,985,"ROOM / ZONE",23,AMBER,500,2)
w+=line(280,976,553,976,CYAN,3)+line(802,976,1118,976,AMBER,3)+text(334,1025,"Wi-Fi / HTTPS",20,MUTED)+text(906,1025,"Hue lighting",20,MUTED)
w+=text(83,1102,"Lighting commands stay on the local network. Bridge discovery: mDNS, then optional internet fallback.",22,MUTED)
w+=text(83,1170,"DOCUMENTED PIN ASSIGNMENTS · TECHNICAL ILLUSTRATION",17,MUTED,400,2)
svg("wiring",1600,1230,w)
print(f"Generated 6 editable SVGs in {OUT}")
