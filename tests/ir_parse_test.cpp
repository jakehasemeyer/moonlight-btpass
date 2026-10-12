#include "../ir_parse.h"
#include <cassert>
#include <cmath>

static void put_pair(uint8_t* p, unsigned x1, unsigned y1, unsigned x2, unsigned y2) {
    p[0] = x1; p[1] = y1; p[2] = ((x1 >> 8) << 4) | ((y1 >> 8) << 6) | (x2 >> 8) | ((y2 >> 8) << 2);
    p[3] = x2; p[4] = y2;
}
int main() {
    uint8_t e[12] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    e[0] = 0xff; e[1] = 0xff; e[2] = 0xf3;
    e[3] = 0xff; e[4] = 0xff; e[5] = 0xb0;
    assert(decode_ir_extended(e).visible && decode_ir_extended(e).x == 0);
    e[3] = 0xff; e[4] = 0xff; e[5] = 0xff;
    assert(!decode_ir_extended(e).visible);
    uint8_t p[10]{};
    put_pair(p, 511, 383, 512, 384);
    put_pair(p + 5, 1023, 1023, 1023, 1023);
    auto a = decode_ir_basic(p);
    assert(a.visible && std::abs(int(a.x) - 959) < 3 && std::abs(int(a.y) - 470) < 3);
    put_pair(p, 1023, 1023, 1023, 1023);
    assert(!decode_ir_basic(p).visible);
    put_pair(p, 0, 0, 1023, 1023);
    a = decode_ir_basic(p);
    assert(a.visible && a.x == 1919 && a.y == 0);
    put_pair(p, 1023, 767, 1023, 1023);
    a = decode_ir_basic(p);
    assert(a.visible && a.x == 0 && a.y == 941);
}
