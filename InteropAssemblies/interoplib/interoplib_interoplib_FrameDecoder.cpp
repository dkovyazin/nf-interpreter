//-----------------------------------------------------------------------------
//
// LEDTREES: нативный декод одного кадра FrameCodec (delta-RLE).
// Формат байт-в-байт зеркалит managed FrameCodec.cs (LedTrees.Device.App) и
// TS-энкодер (npm ledtrees-video-converter, source/frame-codec.ts):
//   [mode:1] [RLE-сегменты, декодирующиеся ровно в frameSize байт]
//   mode 0 — сами байты кадра; mode 1 — XOR-дельта к prev.
//   Сегмент: [H:1] isRun = H & 0x80; n = H & 0x7F; n==0x7F -> n = 0x7F + varint;
//   length = n + 1; RUN -> +1 байт значения; LITERAL -> +length сырых байт.
//
// Интерпретируемые циклы XOR (frameSize итераций на кадр) и RLE на nanoCLR
// занимают ~сотни мс на кадр — здесь микросекунды. Работает только с
// RAM-буферами: managed-обвязка ведёт скользящий буфер, поэтому источником
// может быть и SD-файл, и сетевой поток.
//
// Возврат: >0 — потреблено байт src; -1 — битый формат; -2 — данных не хватило
// (managed дозаполняет буфер и повторяет вызов).
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_FrameDecoder.h"

using namespace interoplib::interoplib;

#define DECODE_ERR_FORMAT -1
#define DECODE_ERR_NEED_DATA -2

signed int FrameDecoder::NativeDecodeFrame( CLR_RT_TypedArray_UINT8 param0, signed int param1, signed int param2, CLR_RT_TypedArray_UINT8 param3, CLR_RT_TypedArray_UINT8 param4, uint16_t param5, HRESULT &hr )
{
    const uint8_t *srcBase = (const uint8_t *)param0.GetBuffer();
    signed int offset = param1;
    signed int count = param2;
    uint8_t *prev = (uint8_t *)param3.GetBuffer();
    uint8_t *frame = (uint8_t *)param4.GetBuffer();
    signed int frameSize = param5;

    // валидация границ managed-массивов: не читаем/не пишем мимо
    // offset/count складываем уже беззнаковыми: у signed int сумма двух больших
    // положительных — переполнение (UB), и проверка вправе быть выкинута компилятором
    if (srcBase == NULL || prev == NULL || frame == NULL ||
        offset < 0 || count < 0 || (uint32_t)offset + (uint32_t)count > param0.GetSize() ||
        frameSize <= 0 || (uint32_t)frameSize > param3.GetSize() || (uint32_t)frameSize > param4.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return DECODE_ERR_FORMAT;
    }

    const uint8_t *src = srcBase + offset;
    signed int pos = 0;

    // mode-байт кадра
    if (pos >= count)
        return DECODE_ERR_NEED_DATA;
    uint8_t mode = src[pos++];
    if (mode > 1)
        return DECODE_ERR_FORMAT;

    signed int out = 0;
    while (out < frameSize)
    {
        if (pos >= count)
            return DECODE_ERR_NEED_DATA;
        uint8_t h = src[pos++];

        bool isRun = (h & 0x80) != 0;
        signed int n = h & 0x7F;
        if (n == 0x7F)
        {
            // varint-удлинение прогона. Копим в беззнаковом: (b & 0x7F) << 28 на
            // signed int задевает знаковый бит — это UB, а на переполнении дальше
            // ломаются проверки границ. У беззнакового лишние биты просто отпадают,
            // а мусор ловит потолок кадра ниже.
            uint32_t v = 0;
            signed int shift = 0;
            while (true)
            {
                if (pos >= count)
                    return DECODE_ERR_NEED_DATA;
                uint8_t b = src[pos++];
                v |= (uint32_t)(b & 0x7F) << shift;
                if (v > (uint32_t)frameSize)
                    return DECODE_ERR_FORMAT; // прогон заведомо длиннее кадра — мусор
                if ((b & 0x80) == 0)
                    break;
                shift += 7;
                if (shift > 28)
                    return DECODE_ERR_FORMAT; // varint длиннее разумного — мусор
            }
            n = 0x7F + (signed int)v;
        }
        signed int len = n + 1;

        // Сверху len держит потолок varint выше (прогон длиннее frameSize — мусор),
        // снизу — единица, так что переполниться len не может; остаётся проверить
        // только выход сегмента за кадр. Сравниваем БЕЗ сложения слева: out + len —
        // потенциальное переполнение signed, а вычитание справа безопасно, т.к. в
        // цикле out < frameSize. Границы проверяем сами: bounds-check CLR
        // (в managed-версии ловил такое как IndexOutOfRangeException) в нативном
        // коде не работает, промах уходит прямо в память за блоком кучи.
        if (len > frameSize - out)
            return DECODE_ERR_FORMAT; // сегмент вылезает за кадр

        if (isRun)
        {
            if (pos >= count)
                return DECODE_ERR_NEED_DATA;
            memset(frame + out, src[pos++], len);
        }
        else
        {
            if (len > count - pos)
                return DECODE_ERR_NEED_DATA;
            memcpy(frame + out, src + pos, len);
            pos += len;
        }

        out += len;
    }

    if (mode == 1)
    {
        // XOR-дельта: восстановить кадр из предыдущего
        for (signed int i = 0; i < frameSize; i++)
            frame[i] ^= prev[i];
    }

    return pos;
}
