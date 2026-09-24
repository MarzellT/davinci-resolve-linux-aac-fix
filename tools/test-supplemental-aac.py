import argparse
import ctypes as c
import os

parser = argparse.ArgumentParser(
    description="Test the supplemental FFmpeg 6.0 AAC decoder with Resolve packet/frame allocators"
)
parser.add_argument("decoder_library", help="Path to the built libavcodec.so.60.3.100")
parser.add_argument(
    "adts_fixture", help="Eight-second stereo 48 kHz AAC-LC ADTS test fixture"
)
cli = parser.parse_args()
bundled_codec = c.CDLL("/opt/resolve/libs/libavcodec.so.60", mode=os.RTLD_GLOBAL)
bundled_util = c.CDLL("/opt/resolve/libs/libavutil.so.58")
supplemental_codec = c.CDLL(cli.decoder_library, mode=os.RTLD_LOCAL | os.RTLD_DEEPBIND)
for lib, name, return_type, argument_types in [
    (supplemental_codec, "avcodec_find_decoder", c.c_void_p, [c.c_int]),
    (supplemental_codec, "avcodec_alloc_context3", c.c_void_p, [c.c_void_p]),
    (
        supplemental_codec,
        "avcodec_open2",
        c.c_int,
        [c.c_void_p, c.c_void_p, c.c_void_p],
    ),
    (supplemental_codec, "avcodec_send_packet", c.c_int, [c.c_void_p, c.c_void_p]),
    (supplemental_codec, "avcodec_receive_frame", c.c_int, [c.c_void_p, c.c_void_p]),
    (supplemental_codec, "avcodec_free_context", None, [c.POINTER(c.c_void_p)]),
    (bundled_util, "av_mallocz", c.c_void_p, [c.c_size_t]),
    (bundled_util, "av_frame_alloc", c.c_void_p, []),
    (bundled_util, "av_frame_free", None, [c.POINTER(c.c_void_p)]),
    (bundled_codec, "av_packet_alloc", c.c_void_p, []),
    (bundled_codec, "av_new_packet", c.c_int, [c.c_void_p, c.c_int]),
    (bundled_codec, "av_packet_unref", None, [c.c_void_p]),
    (bundled_codec, "av_packet_free", None, [c.POINTER(c.c_void_p)]),
]:
    function = getattr(lib, name)
    function.restype = return_type
    function.argtypes = argument_types
# ABI offsets below match the pinned FFmpeg 6.0 libraries.
codec = supplemental_codec.avcodec_find_decoder(0x15002)
ctx = supplemental_codec.avcodec_alloc_context3(codec)
data = bundled_util.av_mallocz(66)
c.memmove(data, bytes.fromhex("1190"), 2)
c.c_void_p.from_address(ctx + 88).value = data
c.c_int.from_address(ctx + 96).value = 2
assert supplemental_codec.avcodec_open2(ctx, codec, None) == 0
packet = bundled_codec.av_packet_alloc()
frame = bundled_util.av_frame_alloc()
adts_data = open(cli.adts_fixture, "rb").read()
position = 0
sample_count = 0
peak = 0
# Feed each ADTS payload through bundled packet/frame allocations.
while position < len(adts_data):
    assert adts_data[position] == 255
    length = (
        ((adts_data[position + 3] & 3) << 11)
        | (adts_data[position + 4] << 3)
        | (adts_data[position + 5] >> 5)
    )
    header_size = 7 if adts_data[position + 1] & 1 else 9
    payload = adts_data[position + header_size : position + length]
    position += length
    assert bundled_codec.av_new_packet(packet, len(payload)) == 0
    c.memmove(c.c_void_p.from_address(packet + 24).value, payload, len(payload))
    assert supplemental_codec.avcodec_send_packet(ctx, packet) == 0
    while supplemental_codec.avcodec_receive_frame(ctx, frame) == 0:
        samples = c.c_int.from_address(frame + 112).value
        sample_format = c.c_int.from_address(frame + 116).value
        assert sample_format == 8
        for channel in range(2):
            plane = c.c_void_p.from_address(frame + channel * 8).value
            values = (c.c_float * samples).from_address(plane)
            peak = max(peak, max(abs(context_pointer) for context_pointer in values))
        sample_count += samples
    bundled_codec.av_packet_unref(packet)
assert peak > 0.1 and sample_count > 380000
print(
    "PASS: supplemental AAC decoder + bundled packet/frame allocators:",
    sample_count,
    "samples; peak",
    peak,
)
packet_pointer = c.c_void_p(packet)
bundled_codec.av_packet_free(c.byref(packet_pointer))
frame_pointer = c.c_void_p(frame)
bundled_util.av_frame_free(c.byref(frame_pointer))
context_pointer = c.c_void_p(ctx)
supplemental_codec.avcodec_free_context(c.byref(context_pointer))
print("PASS: packet, frame, context teardown")
