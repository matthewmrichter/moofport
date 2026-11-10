/* test_util.c: byte access and wrap-safe time helpers. */
#include "t.h"
#include "mp_util.h"

static void test_rd_wr16(void)
{
    uint8_t b[3] = { 0, 0, 0xEE };
    mp_wr16(b, 0xBEEF);
    T_EQ(b[0], 0xBE);
    T_EQ(b[1], 0xEF);
    T_EQ(b[2], 0xEE);
    T_EQ(mp_rd16(b), 0xBEEF);
}

static void test_rd_wr32(void)
{
    uint8_t b[5] = { 0 };
    mp_wr32(b + 1, 0xDEADBEEFUL);   /* odd address: byte access only */
    T_EQ(b[1], 0xDE);
    T_EQ(b[4], 0xEF);
    T_CHECK(mp_rd32(b + 1) == 0xDEADBEEFUL);
    b[1] = 0xFF;
    T_CHECK(mp_rd32(b + 1) == 0xFFADBEEFUL);   /* widened before the shift */
}

static void test_time_wrap(void)
{
    T_CHECK(mp_time_before(1, 2));
    T_CHECK(!mp_time_before(2, 1));
    T_CHECK(!mp_time_before(5, 5));
    T_CHECK(mp_time_before(0xFFFFFFF0UL, 0x10));   /* across the wrap */
    T_CHECK(!mp_time_before(0x10, 0xFFFFFFF0UL));
    T_EQ(mp_time_since(0x10, 0xFFFFFFF0UL), 0x20);
    T_EQ(mp_time_since(5, 10), 0);
}

T_MAIN(test_rd_wr16, test_rd_wr32, test_time_wrap)
