import ctypes as c,os,argparse
parser=argparse.ArgumentParser(description="Test the supplemental FFmpeg 6.0 AAC decoder with Resolve packet/frame allocators")
parser.add_argument("decoder_library", help="Path to the built libavcodec.so.60.3.100")
parser.add_argument("adts_fixture", help="Eight-second stereo 48 kHz AAC-LC ADTS test fixture")
cli=parser.parse_args()
orig=c.CDLL('/opt/resolve/libs/libavcodec.so.60',mode=os.RTLD_GLOBAL)
u=c.CDLL('/opt/resolve/libs/libavutil.so.58')
n=c.CDLL(cli.decoder_library,mode=os.RTLD_LOCAL|os.RTLD_DEEPBIND)
for lib,name,ret,args in [(n,'avcodec_find_decoder',c.c_void_p,[c.c_int]),(n,'avcodec_alloc_context3',c.c_void_p,[c.c_void_p]),(n,'avcodec_open2',c.c_int,[c.c_void_p,c.c_void_p,c.c_void_p]),(n,'avcodec_send_packet',c.c_int,[c.c_void_p,c.c_void_p]),(n,'avcodec_receive_frame',c.c_int,[c.c_void_p,c.c_void_p]),(n,'avcodec_free_context',None,[c.POINTER(c.c_void_p)]),(u,'av_mallocz',c.c_void_p,[c.c_size_t]),(u,'av_frame_alloc',c.c_void_p,[]),(u,'av_frame_free',None,[c.POINTER(c.c_void_p)]),(orig,'av_packet_alloc',c.c_void_p,[]),(orig,'av_new_packet',c.c_int,[c.c_void_p,c.c_int]),(orig,'av_packet_unref',None,[c.c_void_p]),(orig,'av_packet_free',None,[c.POINTER(c.c_void_p)])]:
 f=getattr(lib,name);f.restype=ret;f.argtypes=args
codec=n.avcodec_find_decoder(0x15002);ctx=n.avcodec_alloc_context3(codec);data=u.av_mallocz(66);c.memmove(data,bytes.fromhex('1190'),2)
c.c_void_p.from_address(ctx+88).value=data;c.c_int.from_address(ctx+96).value=2
assert n.avcodec_open2(ctx,codec,None)==0
packet=orig.av_packet_alloc();frame=u.av_frame_alloc();b=open(cli.adts_fixture,'rb').read();pos=0;count=0;peak=0
while pos<len(b):
 assert b[pos]==255
 length=((b[pos+3]&3)<<11)|(b[pos+4]<<3)|(b[pos+5]>>5);hdr=7 if b[pos+1]&1 else 9;payload=b[pos+hdr:pos+length];pos+=length
 assert orig.av_new_packet(packet,len(payload))==0
 c.memmove(c.c_void_p.from_address(packet+24).value,payload,len(payload))
 assert n.avcodec_send_packet(ctx,packet)==0
 while n.avcodec_receive_frame(ctx,frame)==0:
  samples=c.c_int.from_address(frame+112).value;fmt=c.c_int.from_address(frame+116).value
  assert fmt==8
  for ch in range(2):
   plane=c.c_void_p.from_address(frame+ch*8).value;values=(c.c_float*samples).from_address(plane);peak=max(peak,max(abs(x) for x in values))
  count+=samples
 orig.av_packet_unref(packet)
assert peak>.1 and count>380000
print('PASS: supplemental AAC decoder + bundled packet/frame allocators:',count,'samples; peak',peak)
p=c.c_void_p(packet);orig.av_packet_free(c.byref(p));f=c.c_void_p(frame);u.av_frame_free(c.byref(f));x=c.c_void_p(ctx);n.avcodec_free_context(c.byref(x))
print('PASS: packet, frame, context teardown')
