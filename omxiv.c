#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <signal.h>
#include <dirent.h>

#include "omx_render.h"
#include "omx_image.h"
#include "soft_image.h"
#include "bcm_host.h"

#define str(s) #s
#define TO_STR(s) str(s)

static const char magNumJpeg[] = {0xff, 0xd8, 0xff};
static const char magNumPng[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1A, 0x0A};
static const char magNumBmp[] = {0x42, 0x4d};
static const char magNumGif[] = {0x47, 0x49, 0x46, 0x38};

static ILCLIENT_T *client=NULL;
static char end=0;

static void resetTerm(){
	struct termios old = {0};
	if (tcgetattr(0, &old) < 0)
		perror("tcsetattr()");
	old.c_lflag |= ICANON;
	old.c_lflag |= ECHO;
	if (tcsetattr(0, TCSADRAIN, &old) < 0)
		perror("tcsetattr ~ICANON");
}

void sig_handler(int sigNum){
	end=1;
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
}

static int isDir(char *path){
	struct stat statb;
	if(stat(path, &statb)== -1)
		return 0;
	if(S_ISDIR(statb.st_mode))
		return 1;
	else
		return 0;
}

static unsigned long getCurrentTimeMs(){
	struct timeval  tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec) * 1000UL + (tv.tv_usec) / 1000UL ;
}

static void cpyImage(IMAGE *from, IMAGE *to){
	if(to->pData != from->pData)
		free(to->pData);
	to->colorSpace = from->colorSpace;
	to->width = from->width;
	to->height = from->height;
	to->nData = from->nData;
	to->pData = from->pData;
}

static int imageFilter(const struct dirent *entry){
	char* ext = strrchr(entry->d_name, '.');
	if(ext!=NULL && (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".JPG") == 0 ||
			strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".JPEG") == 0 ||
			strcmp(ext, ".jpe") == 0 || strcmp(ext, ".JPE") == 0 ||
			strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0 ||
			strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0 ||
			strcmp(ext, ".gif") == 0 || strcmp(ext, ".GIF") == 0 ))
		return 1;
	else
		return 0;
}

static int getImageFilesInDir(char ***list, char* path){
	struct dirent **namelist;
	int imageNum;
	imageNum = scandir(path, &namelist, imageFilter, alphasort);
	if (imageNum < 0)
		return imageNum;
	else {
		*list=malloc(sizeof(char*) *imageNum);
		int i;
		for(i=0; i<imageNum; i++) {
			if(strcmp(path, ".") == 0 || strcmp(path, "./") == 0){
				(*list)[i]= malloc(strlen(namelist[i]->d_name)+1);
				strcpy((*list)[i], namelist[i]->d_name);
			}else{
				if(strrchr(path, '/')- path != strlen(path)-1){
					(*list)[i]= malloc(strlen(path)+strlen(namelist[i]->d_name)+2);
					strcpy((*list)[i],path);
					(*list)[i][strlen(path)]='/';
					strcpy((*list)[i]+strlen(path)+1,namelist[i]->d_name);
				}else{
					(*list)[i]= malloc(strlen(path)+strlen(namelist[i]->d_name)+1);
					strcpy((*list)[i],path);
					strcpy((*list)[i]+strlen(path),namelist[i]->d_name);
				}
			}
			free(namelist[i]);
		}
		free(namelist);
	}
	return imageNum;
}


void printUsage(const char *progr){
	printf("\n");
	printf("Usage: %s [OPTIONS] image1 [image2] ...\n", progr);
	printf("       %s [OPTIONS] directory\n\n", progr);
	printf("Without any input it will cycle through all\n");
	printf("supported images in the current folder.\n\n");
	printf("OPTIONS:\n\n");
	printf("    -h  --help                  Print this help\n");
	printf("    -v  --version               Show version info\n");
	printf("    -t                  n       Time in s between 2 images in a slide show\n");
	printf("    -b  --blank                 Set background to black\n");
	printf("    -y  --yuv420                Use YUV420 for rendering instead of RGBA\n\n");
	printf("        --win 'x1 y1 x2 y2'     Position of image window\n");
	printf("        --win x1,y1,x2,y2       Position of image window\n");
	printf("    -f  --fill                  Use the whole screen for the image\n");
	printf("    -a  --no-aspect             Don't keep aspect ratio when used with --win\n");
	printf("    -o  --orientation   n       Orientation of the image (0, 90, 180, 270)\n");
	printf("    -m  --mirror                Mirror image\n");
	printf("    -l  --layer         n       Render layer number\n");
	printf("    -d  --display       n       Display number\n");
	printf("    -i  --info                  Print some additional infos\n");
	printf("    -k  --no-keys               Disable keyboard input\n");
	printf("    -s  --soft                  Force software decoding\n\n");
	printf("Key configuration:\n\n");
	printf("    ESC, q  :   Quit\n");
	printf("    LEFT    :   Previous image\n");
	printf("    RIGHT   :   Next image\n");
	printf("    UP      :   Rotate right\n");
	printf("    Down    :   Rotate left\n");
	printf("    m       :   Mirror image\n");
	printf("    p       :   Pause slide show\n");
	printf("\n");
}

// http://stackoverflow.com/a/912796
static char getch(int timeout) {
	char buf = 0;
	struct termios old = {0};
	if (tcgetattr(0, &old) < 0)
		perror("tcsetattr()");
	old.c_lflag &= ~ICANON;
	old.c_lflag &= ~ECHO;
	old.c_cc[VMIN] = 0;
	old.c_cc[VTIME] = timeout;
	if (tcsetattr(0, TCSANOW, &old) < 0)
		perror("tcsetattr ICANON");
	if (read(0, &buf, 1) < 0)
		perror("read()");
	return (buf);
}

static int decodeImage(char *filePath, IMAGE *image, ANIM_IMAGE *anim, char info, 
			char color, OMX_RENDER_DISP_CONF *dispConfig, char soft){
	int ret = 0;
	FILE *imageFile;
	unsigned char *httpImMem = NULL;
	size_t size = 0;
	char magNum[8];
	
#ifdef USE_LIBCURL
	if(strncmp(filePath, "http://", 7) == 0 || strncmp(filePath, "https://", 8) == 0){
		if(info)
			printf("Open Url: %s\n", filePath);
		httpImMem = getImageFromUrl(filePath, &size);
		if(httpImMem == NULL){
			fprintf(stderr, "Couldn't get Image from Url\n");
			return 0x200;
		}
		imageFile = fmemopen((void*) httpImMem, size, "rb");
	}else
#endif	
	{
		if(info)
			printf("Open file: %s\n", filePath);
	
		imageFile = fopen(filePath, "rb");
	}
	
	if(!imageFile){
		return SOFT_IMAGE_ERROR_FILE_OPEN;
	}
	
	if(fread(&magNum, 1, 8, imageFile) != 8){
		fclose(imageFile);
		free(httpImMem);
		return 0x100;
	}
	rewind(imageFile);
	
	if(memcmp(magNum, magNumJpeg, sizeof(magNumJpeg)) == 0){

		JPEG_INFO jInfo;
		ret=readJpegHeader(imageFile, &jInfo);
		if(ret != SOFT_JPEG_OK){
			fclose(imageFile);
			free(httpImMem);
			return ret;
		}
		
		rewind(imageFile);

		if(soft || jInfo.mode == JPEG_MODE_PROGRESSIVE || jInfo.nColorComponents != 3){
			if(info)
				printf("Soft decode jpeg\n");
			ret = softDecodeJpeg(imageFile, image);
		}else{
			if(info)
				printf("Hard decode jpeg\n");
			ret = omxDecodeJpeg(client, imageFile, image);
		}
	}else if(memcmp(magNum, magNumPng, sizeof(magNumPng)) == 0){
		ret = softDecodePng(imageFile, image);
	}else if(memcmp(magNum, magNumBmp, sizeof(magNumBmp)) == 0){
		ret = softDecodeBMP(imageFile, image, &httpImMem, size);
	}else if(memcmp(magNum, magNumGif, sizeof(magNumGif)) == 0){
		ret = softDecodeGif(imageFile, anim, image, &httpImMem, size);
	}else{
		printf("Unsupported image\n");
		fclose(imageFile);
		free(httpImMem);
		return 0x100;
	}
	
	fclose(imageFile);
	free(httpImMem);

	if(info)
		printf("Width: %u, Height: %u\n", image->width, image->height);

	if(ret == 0 && anim->frameCount < 2){		
		IMAGE image2;
		image2.colorSpace = color;
		image2.height = dispConfig->height;
		image2.width = dispConfig->width;
			
		ret = omxAutoResize(client, image, &image2, dispConfig->display, dispConfig->rotation, 
				dispConfig->configFlags & OMX_DISP_CONFIG_FLAG_NO_ASPECT);
		if(ret != OMX_RESIZE_OK){
			printf("resize returned 0x%x\n", ret);
		}else{
			cpyImage(&image2, image);
			if(info)
				printf("Resized Width: %u, Height: %u\n", image->width, image->height);
		}
	}

	return ret;
}

/* From: https://github.com/popcornmix/omxplayer/blob/master/omxplayer.cpp#L455
 * Licensed under the GPLv2 */
static void blankBackground(int imageLayer, int displayNum) {
	// we create a 1x1 black pixel image that is added to display just behind video
	DISPMANX_DISPLAY_HANDLE_T display;
	DISPMANX_UPDATE_HANDLE_T update;
	DISPMANX_RESOURCE_HANDLE_T resource;
	uint32_t vc_image_ptr;
	VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;
	uint16_t image = 0x0000; // black
	int layer = imageLayer-1;

	VC_RECT_T dst_rect, src_rect;

	display = vc_dispmanx_display_open(displayNum);

	resource = vc_dispmanx_resource_create( type, 1 /*width*/, 1 /*height*/, &vc_image_ptr );

	vc_dispmanx_rect_set( &dst_rect, 0, 0, 1, 1);

	vc_dispmanx_resource_write_data( resource, type, sizeof(image), &image, &dst_rect );

	vc_dispmanx_rect_set( &src_rect, 0, 0, 1<<16, 1<<16);
	vc_dispmanx_rect_set( &dst_rect, 0, 0, 0, 0);

	update = vc_dispmanx_update_start(0);

	vc_dispmanx_element_add(update, display, layer, &dst_rect, resource, &src_rect,
									DISPMANX_PROTECTION_NONE, NULL, NULL, DISPMANX_STEREOSCOPIC_MONO );

	vc_dispmanx_update_submit_sync(update);
}

// http://stackoverflow.com/a/3940758
static int isBackgroundProc() {
    pid_t fg = tcgetpgrp(STDIN_FILENO);
    if(fg == -1) {
        return 1;
    }  else if (fg == getpgrp()) {
        return 0;
    } else {
        return 1;
    }
}

static void printVersion(){
	printf("Version: %s\n", TO_STR(VERSION));
	printf("Build date: %s\n", __TIMESTAMP__);
#ifndef USE_LIBCURL
	printf("No libcurl support\n");  
#endif
}

int main(int argc, char *argv[]){

	int ret=1;
	long timeout=0;
	char info=0, blank=0, soft=0, keys=1;
	int initRotation=0;
	char color = COLOR_SPACE_RGBA;

	OMX_RENDER_DISP_CONF dispConfig;
	memset(&dispConfig, 0, sizeof(OMX_RENDER_DISP_CONF));
	dispConfig.mode=OMX_DISPLAY_MODE_LETTERBOX;

	if(isBackgroundProc()){
		keys=0;
	}

	int i;
	for(i=1; i<argc; i++){
		if(argv[i][0]=='-'){
			if(strcmp(argv[i], "--help") == 0){
				printUsage(argv[0]);
				return 0;
			}else if(strcmp(argv[i], "--version") == 0){
				printVersion();
				return 0;
			}else if(strcmp(argv[i], "--blank") == 0){
				blank=1;
			}else if(strcmp(argv[i], "--fill") == 0){
				dispConfig.mode = OMX_DISPLAY_MODE_FILL;
			}else if(strcmp(argv[i], "--no-aspect") == 0){
				dispConfig.configFlags |= OMX_DISP_CONFIG_FLAG_NO_ASPECT;
			}else if(strcmp(argv[i], "--yuv420") == 0){
				color = COLOR_SPACE_YUV420_PACKED;
			}else if(strcmp(argv[i], "--no-keys") == 0){
				keys=0;
			}else if(strcmp(argv[i], "--mirror") == 0){
				dispConfig.configFlags |= OMX_DISP_CONFIG_FLAG_MIRROR;
			}else if(strcmp(argv[i], "--info") == 0){
				info=1;
			}else if(strcmp(argv[i], "--orientation") == 0){
				if(argc > i+1){
					dispConfig.rotation = strtol(argv[++i], NULL, 10);
					initRotation = dispConfig.rotation;
				}else{
					printUsage(argv[0]);
					return 1;
				}
			}else if(strcmp(argv[i], "--layer") == 0){
				if(argc > i+1){
					dispConfig.layer = strtol(argv[++i], NULL, 10);
				}else{
					printUsage(argv[0]);
					return 1;
				}
			}else if(strcmp(argv[i], "--display") == 0){
				if(argc > i+1){
					dispConfig.display = strtol(argv[++i], NULL, 10);
				}else{
					printUsage(argv[0]);
					return 1;
				}
			}else if(strcmp(argv[i], "--win") == 0){
				if(argc > i+1){
					char *pos = strtok (argv[++i],", '");
					dispConfig.xOffset = strtol(pos, NULL, 10);
					pos = strtok (NULL,", ");
					if(pos!=NULL){
						dispConfig.yOffset = strtol(pos, NULL, 10);
						pos = strtok (NULL,", ");
						dispConfig.width = strtol(pos, NULL, 10) - dispConfig.xOffset;
						pos = strtok (NULL,", '");
						dispConfig.height =  strtol(pos, NULL, 10) - dispConfig.yOffset;
					}
				}
			}else if(strcmp(argv[i], "--soft") == 0){
				soft=1;
			}else{
				if(strstr(argv[i], "h") != NULL ){
					printUsage(argv[0]);
					return 0;
				}
				if(strstr(argv[i], "v") != NULL ){
					printVersion();
					return 0;
				}
				if(strstr(argv[i], "b") != NULL)
					blank = 1;
				if(strstr(argv[i], "f") != NULL)
					dispConfig.mode = OMX_DISPLAY_MODE_FILL;
				if(strstr(argv[i], "i") != NULL)
					info = 1;
				if(strstr(argv[i], "a") != NULL)
					dispConfig.configFlags |= OMX_DISP_CONFIG_FLAG_NO_ASPECT;
				if(strstr(argv[i], "m") != NULL)
					dispConfig.configFlags |= OMX_DISP_CONFIG_FLAG_MIRROR;
				if(strstr(argv[i], "y") != NULL)
					color = COLOR_SPACE_YUV420_PACKED;
				if(strstr(argv[i], "s") != NULL)
					soft=1;
				if(strstr(argv[i], "k") != NULL)
					keys=0;
				if(strstr(argv[i], "t") != NULL && argc > i+1)
					timeout = strtol(argv[++i], NULL, 10)*1000;
				if(strstr(argv[i], "l") != NULL && argc > i+1)
					dispConfig.layer = strtol(argv[++i], NULL, 10);
				if(strstr(argv[i], "d") != NULL && argc > i+1)
					dispConfig.display = strtol(argv[++i], NULL, 10);
				if(strstr(argv[i], "o") != NULL && argc > i+1){
					dispConfig.rotation = strtol(argv[++i], NULL, 10);
					initRotation = dispConfig.rotation;
				}
			}
		}else{
			break;
		}
	}

	int imageNum;
	char **files;
	if(argc-i <= 0){
		imageNum=getImageFilesInDir(&files, "./");
	}else if(isDir(argv[i])){
		imageNum=getImageFilesInDir(&files, argv[i]);
	}else{
		imageNum = argc-i;

		files=malloc(sizeof(char*) *imageNum);
		int x;
		for(x =0 ; i+x<argc; x++){
			files[x]=argv[i+x];
		}
	}

	if(imageNum<1){
		fprintf(stderr, "No images to display\n");
		return 1;
	}

	bcm_host_init();

	if ((client = ilclient_init()) == NULL) {
		perror("Error init ilclient\n");
		return 1;
	}

	if (OMX_Init() != OMX_ErrorNone) {
		perror("Error init omx\n");
		ilclient_destroy(client);
		return 1;
	}

	OMX_RENDER render;
	render.client=client;
	unsigned long lShowTime = 0;
	unsigned long cTime;
	IMAGE image = INIT_IMAGE;
	ANIM_IMAGE anim = INIT_ANIM_IMAGE;
	
	ret=decodeImage(files[0], &image, &anim, info, color, &dispConfig, soft);

	if(ret==0){
		if(blank)
			blankBackground(dispConfig.layer, dispConfig.display);
		lShowTime = getCurrentTimeMs();
		if(anim.frameCount < 2){
			ret = renderImage(&render, &image, &dispConfig);
			destroyImage(&image);
		}else{
			ret = renderAnimation(&render, &anim, &dispConfig);
		}
		if(ret != 0){
			fprintf(stderr, "render returned 0x%x\n", ret);
			end=1;
		}
	}else{
		if(ret == SOFT_IMAGE_ERROR_FILE_OPEN){
			fprintf(stderr, "Error file does not exist or is corrupted.\n");
		}else if(ret!= 0x100){
			fprintf(stderr, "decoder returned 0x%x\n", ret);
		}
		end=1;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	i=0;
	char c=0, paused=0;
	while(!end){
		if(keys){
			tcflush(0, TCIFLUSH);
			c = getch(1);
			if(end)
				break;
		}else{
			usleep(20000);
		}
		if(timeout != 0 && imageNum > 1 && !paused){
			cTime = getCurrentTimeMs();
			if( (cTime-lShowTime) > timeout){
				if(imageNum <= ++i)
					i=0;
				dispConfig.rotation= initRotation;
				stopAnimation(&render);
				ret=decodeImage(files[i], &image, &anim, info, color, &dispConfig, soft);
				if(ret==0){
					ret = stopImageRender(&render);
					if(ret != 0){
						fprintf(stderr, "render cleanup returned 0x%x\n", ret);
						break;
					}
					if(anim.frameCount < 2){
						ret = renderImage(&render, &image, &dispConfig);
						destroyImage(&image);
					}else{
						ret = renderAnimation(&render, &anim, &dispConfig);
					}
					if(ret != 0){
						fprintf(stderr, "render returned 0x%x\n", ret);
						break;
					}
					lShowTime = getCurrentTimeMs();
				}
			}
		}

		if(c == 0){
			continue;
		}else if(c == 'q' || c =='Q'){
			break;
		}else if(c == 'm' || c =='M'){
			dispConfig.configFlags^= OMX_DISP_CONFIG_FLAG_MIRROR;
			ret = setDisplayConfig(&render, &dispConfig);
			if(ret != 0){
				fprintf(stderr, "dispConfig set returned 0x%x\n", ret);
				break;
			}
		}else if(c == 0x1b){
			c=getch(1);
			if(c == 0)
				break;
			c = getch(1);
			if(c == 0x41){
				if(dispConfig.rotation>0)
					dispConfig.rotation-= 90;
				else
					dispConfig.rotation=270;
				ret = setDisplayConfig(&render, &dispConfig);
				if(ret != 0){
					fprintf(stderr, "dispConfig set returned 0x%x\n", ret);
					break;
				}
			}else if(c == 0x42){
				dispConfig.rotation+= 90;
				dispConfig.rotation%=360;
				ret = setDisplayConfig(&render, &dispConfig);
				if(ret != 0){
					fprintf(stderr, "dispConfig set returned 0x%x\n", ret);
					break;
				}
			}else if(c == 0x43 && imageNum > 1){
				if(imageNum <= ++i)
					i=0;
				dispConfig.rotation= initRotation;
				stopAnimation(&render);
				ret=decodeImage(files[i], &image, &anim, info, color, &dispConfig, soft);
				if(ret==0){
					ret = stopImageRender(&render);
					if(ret != 0){
						fprintf(stderr, "render cleanup returned 0x%x\n", ret);
						break;
					}
					if(anim.frameCount < 2){
						ret = renderImage(&render, &image, &dispConfig);
						destroyImage(&image);
					}else{
						ret = renderAnimation(&render, &anim, &dispConfig);
					}
					if(ret != 0){
						fprintf(stderr, "render returned 0x%x\n", ret);
						break;
					}
					lShowTime = getCurrentTimeMs();
				}
			}else if(c == 0x44 && imageNum > 1){
				if(0 > --i)
					i=imageNum-1;
				dispConfig.rotation= initRotation;
				stopAnimation(&render);
				ret=decodeImage(files[i], &image, &anim, info, color, &dispConfig, soft);
				if(ret==0){
					ret = stopImageRender(&render);
					if(ret != 0){
						fprintf(stderr, "render cleanup returned 0x%x\n", ret);
						break;
					}
					if(anim.frameCount < 2){
						ret = renderImage(&render, &image, &dispConfig);
						destroyImage(&image);
					}else{
						ret = renderAnimation(&render, &anim, &dispConfig);
					}
					if(ret != 0){
						fprintf(stderr, "render returned 0x%x\n", ret);
						break;
					}
					lShowTime = getCurrentTimeMs();
				}
			}
		}else if(timeout > 0 && (c=='p' || c=='P')){
			paused ^=1;
			if(paused)
				printf("Paused\n");
			else
				printf("Continue\n");
		}
	}

	if(ret == 0){
		ret = stopImageRender(&render);
		if(ret != 0)
			fprintf(stderr, "render cleanup returned 0x%x\n", ret);
	}

	free(image.pData);

	if(keys)
		resetTerm();

	OMX_Deinit();

	if (client != NULL) {
		ilclient_destroy(client);
	}

	bcm_host_deinit();

	return ret;
}