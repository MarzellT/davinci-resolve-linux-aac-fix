#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <errno.h>
#include <pthread.h>

/* This is an isolated native-only port, not the upstream file-conversion shim.
 * The only supported host is Studio 21.1.0.17 Linux x86_64 with this full hash.
 * File bytes, container parsing, packet timestamps and DJI data stay untouched. */
static const unsigned char host_sha[32] = {
 0x99,0xd4,0xe0,0xa4,0x6d,0x63,0xcb,0xfe,0x71,0xce,0x4b,0x30,0xf2,0x3c,0x56,0x54,
 0xff,0xbd,0xd1,0x7d,0x78,0x2d,0x93,0x68,0x98,0x6c,0xec,0xd1,0x59,0xe4,0x15,0xd0};
extern const unsigned char aac_stubs_start[],aac_stubs_end[],aac_dispatch[],aac_lookup[],aac_gate1[],aac_gate2[];
static int active;
struct codec_api {
 const void *(*find)(int);
 void *(*alloc)(const void*);
 int (*open)(void*,const void*,void*);
 int (*send)(void*,const void*);
 int (*receive)(void*,void*);
 void (*flush)(void*);
 int (*close)(void*);
 void (*free_ctx)(void**);
 unsigned int (*version)(void);
};
static struct codec_api original, supplemental;
static const void *supplemental_aac;
static void *supplemental_handle;
static pthread_once_t api_once=PTHREAD_ONCE_INIT;
struct owned_ctx {void *ctx;struct owned_ctx *next;};
static struct owned_ctx *owned;
static pthread_mutex_t owned_lock=PTHREAD_MUTEX_INITIALIZER;
static void load_api(void *handle,struct codec_api *a) {
 a->find=dlsym(handle,"avcodec_find_decoder");
 a->alloc=dlsym(handle,"avcodec_alloc_context3");
 a->open=dlsym(handle,"avcodec_open2");
 a->send=dlsym(handle,"avcodec_send_packet");
 a->receive=dlsym(handle,"avcodec_receive_frame");
 a->flush=dlsym(handle,"avcodec_flush_buffers");
 a->close=dlsym(handle,"avcodec_close");
 a->free_ctx=dlsym(handle,"avcodec_free_context");
 a->version=dlsym(handle,"avcodec_version");
}
static void resolve_api(void) {load_api(RTLD_NEXT,&original);}
static int api_complete(struct codec_api *a) {
 return a->find&&a->alloc&&a->open&&a->send&&a->receive&&a->flush&&a->close&&a->free_ctx&&a->version;
}
static int is_owned(void *ctx) {
 int result=0;pthread_mutex_lock(&owned_lock);
 for(struct owned_ctx *n=owned;n;n=n->next)if(n->ctx==ctx){result=1;break;}
 pthread_mutex_unlock(&owned_lock);return result;
}
static void logmsg(const char *s) { write(2,s,strlen(s)); }
struct patch { uintptr_t va; size_t n; const unsigned char *fp,*stub; unsigned char replacement[16]; };
static const unsigned char fp1[]={0xe9,0xac,0,0,0};
static const unsigned char fp2[]={0x4c,0x8b,0x05,0xd8,0x3d,0x9e,0x23};
static const unsigned char fp3[]={0x81,0xf9,0x10,0x50,0x01,0,0x74,0x0c};
static const unsigned char fp4[]={0x81,0xff,0x10,0x50,0x01,0,0x0f,0x84,0xed,0,0,0};
static struct patch patches[]={
 {0x5dd63da,sizeof fp1,fp1,aac_dispatch,{0}},
 {0x5dcb2b9,sizeof fp2,fp2,aac_lookup,{0}},
 {0x5dcb48f,sizeof fp3,fp3,aac_gate1,{0}},
 {0x5dcb4e5,sizeof fp4,fp4,aac_gate2,{0}},
};
static int hash_host(void) {
 int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC); if(fd<0)return 0;
 SHA256_CTX c; unsigned char digest[32],buf[65536]; ssize_t n;
 SHA256_Init(&c);
 while((n=read(fd,buf,sizeof buf))>0)SHA256_Update(&c,buf,(size_t)n);
 close(fd); if(n<0)return 0; SHA256_Final(digest,&c);
 return !memcmp(digest,host_sha,32);
}
static int page_protection(uintptr_t page,size_t pagesize) {
 FILE *f=fopen("/proc/self/maps","r"); if(!f)return -1;
 char line[PATH_MAX+256],perms[5],path[PATH_MAX]; unsigned long lo,hi,offset,inode; unsigned int maj,min;
 int found=-1;
 while(fgets(line,sizeof line,f)) {
  path[0]=0;
  if(sscanf(line,"%lx-%lx %4s %lx %x:%x %lu %4095s",&lo,&hi,perms,&offset,&maj,&min,&inode,path)==8 &&
     page>=lo && page+pagesize<=hi && !strcmp(path,"/opt/resolve/bin/resolve")) {
   found=(perms[0]=='r'?PROT_READ:0)|(perms[1]=='w'?PROT_WRITE:0)|(perms[2]=='x'?PROT_EXEC:0); break;
  }
 }
 fclose(f); return found;
}
__attribute__((constructor)) static void init_native(void) {
 char exe[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);
 if(n<0)return;
 exe[n]=0;
 if(strcmp(exe,"/opt/resolve/bin/resolve"))return;
 if(!hash_host()) {logmsg("[aac_native21] Unsupported Resolve SHA256; no patches applied.\n");return;}
 pthread_once(&api_once,resolve_api);
 if(!api_complete(&original) || (original.version()>>16)!=60) {
  logmsg("[aac_native21] Unsupported decoder ABI; no patches applied.\n");return;
 }
 if(!original.find(0x15002)) {
  const char *path=getenv("RESOLVE_AAC_DECODER_LIBRARY");
  if(!path||path[0]!='/') {
   logmsg("[aac_native21] Bundled AAC decoder absent; supplemental library required. No patches applied.\n");return;
  }
  /* A separate codec implementation with the exact FFmpeg 6.0 ABI, kept local.
   * Existing libavutil58 stays shared for AVFrame and buffer allocation.
   * Deep binding keeps the supplemental codec's internal calls self-contained.
   * Only registered AAC contexts are ever routed to it; all video/other audio
   * functions continue to use the original bundled codec implementation. */
  supplemental_handle=dlopen(path,RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND);
  if(supplemental_handle)load_api(supplemental_handle,&supplemental);
  if(!api_complete(&supplemental)||supplemental.version()!=((60u<<16)|(3u<<8)|100u)||
     !(supplemental_aac=supplemental.find(0x15002))) {
   logmsg("[aac_native21] Supplemental AAC decoder failed ABI/load checks; no patches applied.\n");
   if(!supplemental_handle) {const char *err=dlerror();if(err)logmsg(err);}
   return;
  }
  logmsg("[aac_native21] Supplemental FFmpeg 6.0 AAC decoder loaded locally; existing codecs retained.\n");
 }
 size_t ps=(size_t)sysconf(_SC_PAGESIZE),count=sizeof patches/sizeof patches[0];
 uintptr_t pages[4]; int prot[4]; size_t np=0;
 for(size_t i=0;i<count;i++) {
  struct patch *p=&patches[i];uintptr_t page=p->va&~(ps-1);
  int pr=page_protection(page,ps);
  if(pr!=(PROT_READ|PROT_EXEC) || (p->va+p->n-1)/ps!=p->va/ps || memcmp((void*)p->va,p->fp,p->n)) {
   logmsg("[aac_native21] Fingerprint/protection preflight failed; no patches applied.\n");return;
  }
  size_t j;for(j=0;j<np && pages[j]!=page;j++);
  if(j==np){pages[np]=page;prot[np++]=pr;}
 }
 size_t sz=(size_t)(aac_stubs_end-aac_stubs_start),alloc=(sz+ps-1)&~(ps-1);
 unsigned char *stub=mmap(NULL,alloc,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 if(stub==MAP_FAILED){logmsg("[aac_native21] Cannot allocate trampolines; no patches applied.\n");return;}
 memcpy(stub,aac_stubs_start,sz);
 if(mprotect(stub,alloc,PROT_READ|PROT_EXEC)){munmap(stub,alloc);return;}
 for(size_t i=0;i<count;i++) {
  struct patch *p=&patches[i]; intptr_t dst=(intptr_t)(stub+(p->stub-aac_stubs_start));
  int64_t delta=dst-(int64_t)(p->va+5);
  if(delta<INT32_MIN||delta>INT32_MAX){munmap(stub,alloc);return;}
  memset(p->replacement,0x90,p->n);p->replacement[0]=0xe9;
  int32_t rel=(int32_t)delta;memcpy(p->replacement+1,&rel,4);
 }
 size_t changed=0;
 for(;changed<np;changed++) if(mprotect((void*)pages[changed],ps,PROT_READ|PROT_WRITE))break;
 if(changed!=np) {
  for(size_t j=0;j<changed;j++)if(mprotect((void*)pages[j],ps,prot[j]))_exit(125);
  munmap(stub,alloc);logmsg("[aac_native21] Cannot make patch pages writable; no patches applied.\n");return;
 }
 for(size_t i=0;i<count;i++)memcpy((void*)patches[i].va,patches[i].replacement,patches[i].n);
 for(size_t j=0;j<np;j++) if(mprotect((void*)pages[j],ps,prot[j])) {
  /* Never continue execution with a partially applied patch or altered W^X. */
  logmsg("[aac_native21] Fatal page protection restoration error.\n");_exit(125);
 }
 active=1;
 logmsg("[aac_native21] Native AAC port active: Studio 21.1.0.17, four verified detours, original RX protections restored. No file redirection.\n");
}

/* ISO/IEC 14496-1 expandable lengths. Bound every field against its parent's
 * descriptor, support 1..4-byte lengths and the optional ES header fields. */
static int descriptor(const unsigned char *b,size_t end,size_t *pos,unsigned int want,size_t *stop) {
 if(*pos>=end||b[(*pos)++]!=want)return 0;
 size_t len=0;unsigned int k;
 for(k=0;k<4;k++) {if(*pos>=end)return 0;unsigned char c=b[(*pos)++];len=(len<<7)|(c&127);if(!(c&128))break;}
 if(k==4||len>end-*pos)return 0;
 *stop=*pos+len;return 1;
}
int aac_extract_asc(const unsigned char *b,size_t n,size_t *off,size_t *len) {
 /* Resolve supplies the esds full-box payload (version/flags then ES tag),
  * but tolerate an outer esds atom and tag-only payload for a pure parser test. */
 size_t p=0,end=n,esend,dcend,ascend;
 if(n>=8&&!memcmp(b+4,"esds",4)){
  size_t box=((size_t)b[0]<<24)|((size_t)b[1]<<16)|((size_t)b[2]<<8)|b[3];
  if(box<12||box>n)return 0;
  end=box;p=8;
 }
 if(end-p>=5 && b[p]==0 && b[p+1]==0 && b[p+2]==0 && b[p+3]==0)p+=4;
 if(!descriptor(b,end,&p,3,&esend)||esend-p<3)return 0;
 p+=2;unsigned char flags=b[p++];
 if(flags&128){if(esend-p<2)return 0;p+=2;}
 if(flags&64){if(p>=esend)return 0;size_t z=b[p++];if(z>esend-p)return 0;p+=z;}
 if(flags&32){if(esend-p<2)return 0;p+=2;}
 if(!descriptor(b,esend,&p,4,&dcend)||dcend-p<13)return 0;
 if(b[p]!=0x40 || ((b[p+1]>>2)&63)!=5)return 0; /* MPEG-4 audio */
 p+=13;
 if(!descriptor(b,dcend,&p,5,&ascend)||ascend-p<2)return 0;
 *off=p;*len=ascend-p;return 1;
}
/* libavcodec60 AVCodecContext ABI used by this exact Resolve build. These
 * offsets are independently evident in its allocator/copy/open instructions. */
int avcodec_open2(void *ctx,const void *codec,void *opts) {
 pthread_once(&api_once,resolve_api);
 if(!original.open)return -ENOSYS;
 if(active && __builtin_return_address(0)==(void*)0x5dcb6d1 && ctx && *(int*)((char*)ctx+24)==0x15002) {
  unsigned char *data=*(unsigned char**)((char*)ctx+88);
  int *size=(int*)((char*)ctx+96);
  if(data && *size>0) {
   size_t off=0,len=0;
   if(aac_extract_asc(data,(size_t)*size,&off,&len)) {
    memmove(data,data+off,len);memset(data+len,0,(size_t)*size-len);*size=(int)len;
    logmsg("[aac_native21] Extracted bounded AAC AudioSpecificConfig; opening native decoder.\n");
   } else {
    logmsg("[aac_native21] AAC descriptor rejected; refusing unsafe decoder input.\n");return -EINVAL;
   }
  } else {logmsg("[aac_native21] AAC extradata missing; refusing decoder.\n");return -EINVAL;}
 }
 return is_owned(ctx)?supplemental.open(ctx,codec,opts):original.open(ctx,codec,opts);
}

/* Public libavcodec entry points used by Resolve's audio wrapper. */
const void *avcodec_find_decoder(int id) {
 pthread_once(&api_once,resolve_api);
 if(active && supplemental_aac && id==0x15002 && __builtin_return_address(0)==(void*)0x5dcb46b)return supplemental_aac;
 return original.find?original.find(id):NULL;
}
void *avcodec_alloc_context3(const void *codec) {
 pthread_once(&api_once,resolve_api);
 if(active && supplemental_aac && codec==supplemental_aac) {
  struct owned_ctx *n=malloc(sizeof *n);if(!n)return NULL;
  void *ctx=supplemental.alloc(codec);if(!ctx){free(n);return NULL;}
  n->ctx=ctx;pthread_mutex_lock(&owned_lock);n->next=owned;owned=n;pthread_mutex_unlock(&owned_lock);return ctx;
 }
 return original.alloc?original.alloc(codec):NULL;
}
int avcodec_send_packet(void *ctx,const void *packet) {
 pthread_once(&api_once,resolve_api);
 return is_owned(ctx)?supplemental.send(ctx,packet):(original.send?original.send(ctx,packet):-ENOSYS);
}
int avcodec_receive_frame(void *ctx,void *frame) {
 pthread_once(&api_once,resolve_api);
 return is_owned(ctx)?supplemental.receive(ctx,frame):(original.receive?original.receive(ctx,frame):-ENOSYS);
}
void avcodec_flush_buffers(void *ctx) {
 pthread_once(&api_once,resolve_api);
 if(is_owned(ctx))supplemental.flush(ctx);else if(original.flush)original.flush(ctx);
}
int avcodec_close(void *ctx) {
 pthread_once(&api_once,resolve_api);
 return is_owned(ctx)?supplemental.close(ctx):(original.close?original.close(ctx):-ENOSYS);
}
void avcodec_free_context(void **ctx) {
 pthread_once(&api_once,resolve_api);
 struct owned_ctx *removed=NULL;
 if(ctx&&*ctx){
  pthread_mutex_lock(&owned_lock);
  struct owned_ctx **p=&owned;
  while(*p){if((*p)->ctx==*ctx){removed=*p;*p=removed->next;break;}p=&(*p)->next;}
  pthread_mutex_unlock(&owned_lock);
 }
 if(removed){supplemental.free_ctx(ctx);free(removed);}else if(original.free_ctx)original.free_ctx(ctx);
}
