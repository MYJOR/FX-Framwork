#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char**argv){
 cgltf_options o={0}; cgltf_data*d; cgltf_parse_file(&o,argv[1],&d); cgltf_load_buffers(&o,d,argv[1]);
 for(int a=2;a<argc;a++){
  for(cgltf_size i=0;i<d->animations_count;i++){ cgltf_animation*an=&d->animations[i]; if(strcmp(an->name,argv[a]))continue;
   printf("%s\n",an->name);
   for(cgltf_size k=0;k<an->channels_count;k++){ cgltf_animation_channel*ch=&an->channels[k]; if(!ch->target_node||strcmp(ch->target_node->name,"root"))continue;
     cgltf_accessor*in=ch->sampler->input,*out=ch->sampler->output; int comps=ch->target_path==cgltf_animation_path_type_rotation?4:3;
     printf(" root path=%d n=%zu dur=%.3f\n",ch->target_path,in->count,0.0);
     for(size_t f=0;f<in->count;f+= (in->count>6? in->count/5:1)){ float t,v[4]; cgltf_accessor_read_float(in,f,&t,1); cgltf_accessor_read_float(out,f,v,comps); printf("  t=%.2f %.3f %.3f %.3f %.3f\n",t,v[0],v[1],v[2],comps==4?v[3]:0);} }
  }}
 return 0;}
