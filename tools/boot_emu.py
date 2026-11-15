# Boot the stock Kali firmware in Unicorn and log every GPIO / ADC / SPI / I2C / UART register write.
# Usage: python boot_emu.py <firmware.bin> [max_instructions]
import sys, struct, collections
from unicorn import *
from unicorn.arm_const import *

path = sys.argv[1]
MAXI = int(sys.argv[2]) if len(sys.argv) > 2 else 60_000_000
img = open(path, 'rb').read()
sp, pc = struct.unpack('<II', img[:8])
base = pc & 0xFF000000
if base == 0x08000000: base = 0x08000000
elif base == 0x24000000: base = 0x24000000
elif base == 0x90000000: base = 0x90040000

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
try:
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
except Exception:
    pass
regions = [(0x00000000, 0x10000), (0x08000000, 0x200000), (0x1FF00000, 0x100000),
           (0x20000000, 0x20000), (0x24000000, 0x80000), (0x30000000, 0x48000),
           (0x38000000, 0x10000), (0x38800000, 0x1000), (0x90000000, 0x800000),
           (0xC0000000, 0x4000000), (0x40000000, 0x20000000), (0xE0000000, 0x100000)]
for a, s in regions:
    uc.mem_map(a, s)
uc.mem_write(base, img)
if base != 0x08000000:
    pass

PER_LO, PER_HI = 0x40000000, 0x60000000
GPIO0 = 0x58020000
names = {0x00: 'MODER', 0x04: 'OTYPER', 0x08: 'OSPEEDR', 0x0C: 'PUPDR', 0x18: 'BSRR', 0x20: 'AFRL', 0x24: 'AFRH'}
gpio = collections.defaultdict(dict)    # (port,pin) -> {mode, pupd, af, pcs}
events = []
reads = collections.Counter()
alt = collections.Counter()
cnt = {}
last_pc = [0]

def port_name(p): return 'P' + 'ABCDEFGHIJK'[p]

shadow = {}
since_write = collections.Counter()
def on_write(uc, access, addr, size, value, ud):
    if not (PER_LO <= addr < PER_HI): return
    a4 = addr & ~3
    if size == 4: shadow[a4] = value & 0xFFFFFFFF
    else:
        sh = (addr & 3) * 8; m = ((1 << (8 * size)) - 1) << sh
        shadow[a4] = (shadow.get(a4, 0) & ~m) | ((value << sh) & m)
    since_write[a4] = 0
    pcv = uc.reg_read(UC_ARM_REG_PC)
    if GPIO0 <= addr < GPIO0 + 11 * 0x400:
        port = (addr - GPIO0) >> 10
        off = (addr - GPIO0) & 0x3FF
        old = struct.unpack('<I', uc.mem_read(addr & ~3, 4))[0]
        if off in (0x00, 0x0C, 0x20, 0x24, 0x04):
            for pin in range(16):
                if off == 0x00 and ((old ^ value) >> (2 * pin)) & 3:
                    gpio[(port, pin)]['mode'] = ['IN', 'OUT', 'AF', 'ANALOG'][(value >> (2 * pin)) & 3]
                    gpio[(port, pin)].setdefault('pcs', []).append(hex(pcv))
                if off == 0x0C and ((old ^ value) >> (2 * pin)) & 3:
                    gpio[(port, pin)]['pupd'] = ['none', 'PU', 'PD', '?'][(value >> (2 * pin)) & 3]
                if off == 0x04 and ((old ^ value) >> pin) & 1:
                    gpio[(port, pin)]['otype'] = 'OD' if (value >> pin) & 1 else 'PP'
            if off in (0x20, 0x24):
                for k in range(8):
                    if ((old ^ value) >> (4 * k)) & 0xF:
                        pin = k + (8 if off == 0x24 else 0)
                        gpio[(port, pin)]['af'] = (value >> (4 * k)) & 0xF
    else:
        events.append((hex(pcv), hex(addr), hex(value)))

def on_read(uc, access, addr, size, value, ud):
    if not (PER_LO <= addr < PER_HI): return
    reads[addr] += 1
    a = addr & ~3
    since_write[a] += 1
    cur = shadow.get(a, struct.unpack('<I', uc.mem_read(a, 4))[0])
    uc.mem_write(a, struct.pack('<I', cur))
    if a == 0x58024410:   # RCC_CFGR: SWS mirrors SW
        sw = cur & 7
        uc.mem_write(a, struct.pack('<I', (cur & ~0x38) | (sw << 3)))
        return
    if a in (0x40005418, 0x40005818):   # I2C ISR: TXE|TXIS|TC|STOPF, no NACK/BUSY
        uc.mem_write(a, struct.pack('<I', 0x63)); return
    if (a & 0xFFFF03FF) == 0x40000024 or a in (0x40000024, 0x40000424, 0x40000824, 0x40000C24):  # TIMx CNT: count up
        cnt[a] = (cnt.get(a, 0) + 1000) & 0xFFFFFFFF
        uc.mem_write(a, struct.pack('<I', cnt[a]))
        return
    if since_write[a] > 8:
        alt[addr] += 1
        v = 0xFFFFFFFF if alt[addr] & 1 else 0
        uc.mem_write(a, struct.pack('<I', v))

uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=PER_LO, end=PER_HI)
uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=PER_LO, end=PER_HI)

# Keep SysTick / DWT counters moving so timeouts expire
ticks = [0]
TICK = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
def on_block(uc, addr, size, ud):
    ticks[0] += 1
    if TICK and ticks[0] % 50 == 0:
        v = struct.unpack('<I', uc.mem_read(TICK, 4))[0]
        uc.mem_write(TICK, struct.pack('<I', v + 1))
    if ticks[0] % 200 == 0:
        uc.mem_write(0xE0001004, struct.pack('<I', (ticks[0] * 50) & 0xFFFFFFFF))  # DWT CYCCNT
uc.hook_add(UC_HOOK_BLOCK, on_block)

# Force success at 'bl f; cbz r0, ok; b .' error-handler sites
import capstone
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
sites = []
for off in range(0, len(img) - 4, 2):
    h = struct.unpack('<H', img[off:off+2])[0]
    if (h & 0xFD07) == 0xB100 and img[off+2:off+4] == b'\xfe\xe7':   # cbz r0 ; b .
        sites.append(base + off)
def force_ok(uc, addr, size, ud):
    uc.reg_write(UC_ARM_REG_R0, 0)
for off in range(4, len(img) - 2, 2):
    h = struct.unpack('<H', img[off:off+2])[0]
    if (h & 0xFF00) == 0xBE00 or (h == 0xE7FE):
        # preceding: cmp r0,#0 (0x2800) ; beq (0xD0xx)
        p1 = struct.unpack('<H', img[off-2:off])[0]; p2 = struct.unpack('<H', img[off-4:off-2])[0]
        if (p1 & 0xFF00) == 0xD000 and p2 == 0x2800:
            sites.append(base + off - 4)
        if (p1 & 0xFD07) == 0xB100:
            sites.append(base + off - 2)
def is_trap(addr):
    o = addr - base
    if not (0 <= o < len(img) - 2): return False
    h = struct.unpack('<H', img[o:o+2])[0]
    return (h & 0xFF00) == 0xBE00 or h == 0xE7FE
for off in range(0, len(img) - 6, 2):
    if struct.unpack('<H', img[off:off+2])[0] != 0x2800: continue
    b = off + 2; h1 = struct.unpack('<H', img[b:b+2])[0]; h2 = struct.unpack('<H', img[b+2:b+4])[0]
    tgt = None
    if (h1 & 0xFF00) == 0xD100:                       # bne short
        imm = (h1 & 0xFF); imm = imm - 256 if imm & 0x80 else imm
        tgt = base + b + 4 + imm * 2
    elif (h1 & 0xFBC0) == 0xF040 and (h2 & 0xD000) == 0x8000:   # bne.w (T3, cond=1)
        S = (h1 >> 10) & 1; imm6 = h1 & 0x3F; J1 = (h2 >> 13) & 1; J2 = (h2 >> 11) & 1; imm11 = h2 & 0x7FF
        imm = (S << 20) | (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1)
        if S: imm -= 1 << 21
        tgt = base + b + 4 + imm
    if tgt is not None and is_trap(tgt): sites.append(base + off)
for s in sites:
    uc.hook_add(UC_HOOK_CODE, force_ok, begin=s, end=s)
print('error sites hooked:', len(sites))
calls = []
skipped = collections.Counter()
def on_invalid(uc, ud):
    pc_ = uc.reg_read(UC_ARM_REG_PC)
    skipped[pc_] += 1
    uc.reg_write(UC_ARM_REG_PC, (pc_ + 4) | 1)
    return True
uc.hook_add(UC_HOOK_INSN_INVALID, on_invalid)
GPIO_INIT = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
def on_gpio_init(uc, addr, size, ud):
    r0 = uc.reg_read(UC_ARM_REG_R0); r1 = uc.reg_read(UC_ARM_REG_R1); lr = uc.reg_read(UC_ARM_REG_LR)
    pinmask, mode, pull, speed, af = struct.unpack('<5I', uc.mem_read(r1, 20))
    port = (r0 - GPIO0) >> 10
    pins = [i for i in range(16) if pinmask >> i & 1]
    sp_ = uc.reg_read(UC_ARM_REG_SP)
    stack = [hex(x) for x in struct.unpack('<16I', uc.mem_read(sp_, 64)) if 0x08000000 <= x < 0x08020000]
    calls.append((port, pins, mode, pull, af, hex(lr), stack[:6]))
if GPIO_INIT:
    uc.hook_add(UC_HOOK_CODE, on_gpio_init, begin=GPIO_INIT, end=GPIO_INIT)
uc.reg_write(UC_ARM_REG_SP, sp)
try:
    uc.emu_start(pc | 1, 0xFFFFFFFF, count=MAXI)
except UcError as e:
    print('stopped:', e, 'pc', hex(uc.reg_read(UC_ARM_REG_PC)))
print('skipped insns', len(skipped), sum(skipped.values()))
print('final pc', hex(uc.reg_read(UC_ARM_REG_PC)))

PSM = {('A',1):'A2 UART1 RX / ADC9',('A',0):'A3 UART1 TX / ADC10',('B',14):'A8 USB DM',('B',15):'A9 USB DP',
       ('C',14):'B5 GATE OUT 1',('C',13):'B6 GATE OUT 2',('B',8):'B7 I2C1 SCL',('B',9):'B8 I2C1 SDA',
       ('G',14):'B9 GATE IN 2',('G',13):'B10 GATE IN 1',('A',5):'C1 CV OUT 2',('A',7):'C2 CV IN 4',
       ('A',2):'C3 CV IN 3',('A',6):'C4 CV IN 2',('A',3):'C5 CV IN 1',('B',1):'C6 CV IN 5',('C',4):'C7 CV IN 6',
       ('C',0):'C8 CV IN 7',('C',1):'C9 CV IN 8',('A',4):'C10 CV OUT 1',('B',4):'D1 SPI2 CS',
       ('C',11):'D2',('C',10):'D3',('C',9):'D4',('C',8):'D5',('C',12):'D6',('D',2):'D7',
       ('C',2):'D8 SPI2 MISO / ADC12',('C',3):'D9 SPI2 MOSI / ADC11',('D',3):'D10 SPI2 SCK',('C',7):'user LED'}
MODES = {0:'IN',1:'OUT_PP',0x11:'OUT_OD',2:'AF_PP',0x12:'AF_OD',3:'ANALOG'}
print('\nHAL_GPIO_Init calls in order:')
for port, pins, mode, pull, af, lr, stack in calls:
    for pin in pins:
        key = ('ABCDEFGHIJK'[port], pin)
        print(f"  P{key[0]}{pin:<3} {PSM.get(key,''):<22} mode={MODES.get(mode, hex(mode)):<7} pull={['none','PU','PD'][pull] if pull<3 else pull} af={af:<3} lr={lr} stack={' '.join(stack)}")
print('\nGPIO configuration (Patch SM header pins marked):')
for (port, pin), d in sorted(gpio.items()):
    key = ('ABCDEFGHIJK'[port], pin)
    tag = PSM.get(key, '')
    print(f"  {port_name(port)}{pin:<3} {tag:<22} mode={d.get('mode','-'):<6} pupd={d.get('pupd','-'):<4} af={d.get('af','-')!s:<3} {d.get('otype','')} first_pc={d.get('pcs',['-'])[0]}")
# Peripheral register writes summarised by block
blocks = collections.OrderedDict()
for pcv, a, v in events:
    blk = int(a, 16) & ~0x3FF
    blocks.setdefault(blk, []).append((pcv, a, v))
PNAMES = {0x40003800:'SPI2',0x40013000:'SPI1',0x40003C00:'SPI3',0x40005400:'I2C1',0x40005800:'I2C2',0x40005C00:'I2C3',
          0x40011000:'USART1',0x40004400:'USART2',0x40022000:'ADC1/2',0x58026000:'ADC3',0x40007400:'DAC1',
          0x40000000:'TIM2',0x40000400:'TIM3',0x40000800:'TIM4',0x40000C00:'TIM5',0x40010000:'TIM1',0x40010400:'TIM8',
          0x58024400:'RCC',0x58024800:'PWR',0x52004000:'QSPI',0x52002000:'FLASH',0x40015800:'SAI1',0x52004000:'QSPI',
          0x48020000:'BDMA?',0x40020000:'DMA1',0x40020400:'DMA2',0x40020800:'DMAMUX1',0x58025400:'BDMA',0x58025800:'DMAMUX2',0x52005000:'FMC',
          0x40012000:'TIM15?',0x40014000:'TIM15',0x40014400:'TIM16',0x40014800:'TIM17',0x40001000:'TIM6',0x40001400:'TIM7'}
print('\nPeripheral blocks written:')
for blk, ev in blocks.items():
    print(f"  {hex(blk)} {PNAMES.get(blk,'?'):<8} {len(ev)} writes")
import json
json.dump({'events': events}, open(path.split('/')[-1] + '.events.json', 'w'))

# Optional: time one function after boot (CALL_FN=addr, e.g. the audio callback), instruction count per call.
import os
if os.environ.get('CALL_FN'):
    fn = int(os.environ['CALL_FN'], 16)
    SCR = 0x24078000
    import math
    bufs = [SCR + 0x100 + i * 0x200 for i in range(4)]  # inL inR outL outR (48 floats each)
    for b in bufs[:2]:
        uc.mem_write(b, struct.pack('<48f', *[0.5 * math.sin(i * 0.3) for i in range(48)]))
    uc.mem_write(SCR, struct.pack('<2I', bufs[0], bufs[1]))
    uc.mem_write(SCR + 8, struct.pack('<2I', bufs[2], bufs[3]))
    count = [0]
    prof = {}
    def count_insn(uc, a, s, u):
        count[0] += 1
        prof[a] = prof.get(a, 0) + 1
    h = uc.hook_add(UC_HOOK_CODE, count_insn)
    RET = 0x24077000
    uc.mem_write(RET, b'\xfe\xe7')  # b . as a landing pad
    res = []
    for k in range(6):
        count[0] = 0
        uc.reg_write(UC_ARM_REG_R0, SCR); uc.reg_write(UC_ARM_REG_R1, SCR + 8); uc.reg_write(UC_ARM_REG_R2, 48)
        uc.reg_write(UC_ARM_REG_LR, RET | 1); uc.reg_write(UC_ARM_REG_SP, 0x2001F000)
        try:
            uc.emu_start(fn | 1, RET, count=5_000_000)
        except UcError as e:
            print('call error', e, hex(uc.reg_read(UC_ARM_REG_PC)))
        res.append(count[0])
    out = struct.unpack('<4f', uc.mem_read(bufs[2], 16))
    print('callback instructions per 48-sample block:', res, 'out sample', out)
    if os.environ.get('PROF_OUT'):
        with open(os.environ['PROF_OUT'], 'w') as f:
            for a, c in sorted(prof.items()):
                f.write('%x %d\n' % (a, c))
