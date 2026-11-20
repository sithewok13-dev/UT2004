// SDLDrv creates the window and the ES context, through ANGLE on Windows. The device draws into
// its own framebuffers. The world's post processing applies gamma, so it works without OS gamma
// ramps and leaves the HUD and menus alone.

#include "OpenGLDrv.h"

IMPLEMENT_CLASS(UOpenGLRenderDevice);

UOpenGLRenderDevice::UOpenGLRenderDevice()
:	RenderInterface(this)
,	DynamicVertexBuffer(GL_ARRAY_BUFFER)
,	DynamicIndexBuffer(GL_ELEMENT_ARRAY_BUFFER)
{
	LockedViewport	= NULL;
	ContextReady	= 0;
	VertexArray = EmptyVertexArray = SceneBuffer = QuadIndices = VertexShaderObject = 0;
	CopyProgram = OcclusionProgram = CompositeProgram = DepthOfFieldProgram = VolumetricProgram = ResolveProgram = OverlayProgram = IndirectProgram = ShadowAtlas = SunShadow = 0;
	FrameFence = NULL;
	DrawElementsBaseVertex = NULL;
	ShadowLight = NULL;
	FrameCount = 0;
	PrevViewValid = FrameViewValid = 0;
	FlushShadows();
}

void UOpenGLRenderDevice::StaticConstructor()
{
	new(GetClass(),TEXT("UseVSync"),		RF_Public)UBoolProperty(CPP_PROPERTY(UseVSync),			TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("ReduceMouseLag"),	RF_Public)UBoolProperty(CPP_PROPERTY(ReduceMouseLag),	TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("Antialiasing"),	RF_Public)UBoolProperty(CPP_PROPERTY(Antialiasing),		TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("AmbientOcclusion"),RF_Public)UBoolProperty(CPP_PROPERTY(AmbientOcclusion),	TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("ContactShadows"),	RF_Public)UBoolProperty(CPP_PROPERTY(ContactShadows),	TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("ScreenSpaceReflections"),RF_Public)UBoolProperty(CPP_PROPERTY(ScreenSpaceReflections),TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("DepthOfField"),	RF_Public)UBoolProperty(CPP_PROPERTY(DepthOfField),		TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("VolumetricFog"),	RF_Public)UBoolProperty(CPP_PROPERTY(VolumetricFog),	TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("GlobalIllumination"),RF_Public)UBoolProperty(CPP_PROPERTY(GlobalIllumination),TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("HeatHaze"),		RF_Public)UBoolProperty(CPP_PROPERTY(HeatHaze),			TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("MotionBlur"),		RF_Public)UBoolProperty(CPP_PROPERTY(MotionBlur),		TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("ResolutionScale"),	RF_Public)UIntProperty(CPP_PROPERTY(ResolutionScale),	TEXT("Options"),CPF_Config);
	new(GetClass(),TEXT("DetectSettings"),	RF_Public)UBoolProperty(CPP_PROPERTY(DetectSettings),	TEXT("Options"),CPF_Config);

	// This runs on the default object before the config loads, so inis without the keys get these.
	Antialiasing = AmbientOcclusion = ContactShadows = ScreenSpaceReflections = DepthOfField = VolumetricFog = GlobalIllumination = HeatHaze = 1;
	ResolutionScale = 100;
}

// Guessed from the renderer's name. 0 affords every effect, 1 is an integrated desktop GPU, 2 a weak phone GPU, 3 a software rasterizer.
static INT GpuTier(const TCHAR* Renderer,UBOOL Capable)
{
	static const struct { const TCHAR* Name; INT Tier; } Known[] =
	{
		{ TEXT("llvmpipe"), 3 }, { TEXT("softpipe"), 3 }, { TEXT("SwiftShader"), 3 }, { TEXT("Basic Render"), 3 },
		{ TEXT("Mali-4"), 2 }, { TEXT("Mali-T"), 2 }, { TEXT("Mali-G3"), 2 }, { TEXT("Mali-G5"), 2 }, { TEXT("PowerVR"), 2 }, { TEXT("VideoCore"), 2 },
		{ TEXT("HD Graphics"), 1 },
	};
	for( INT i=0; i<ARRAY_COUNT(Known); i++ )
		if( appStrstr(Renderer,Known[i].Name) )
			return Known[i].Tier;
	// Adreno 6xx starts to cope at the 640.
	const TCHAR* Adreno = appStrstr(Renderer,TEXT("Adreno (TM) "));
	if( Adreno && appAtoi(Adreno + 12) < 640 )
		return 2;
	return Capable ? 0 : 2;
}

// Lowers the first run's options to what the GPU is likely to afford. It never raises one.
void UOpenGLRenderDevice::DetectOptions()
{
	checkSlow(GpuTier(TEXT("ANGLE (NVIDIA, NVIDIA GeForce RTX 3070 Direct3D11 vs_5_0 ps_5_0, D3D11)"),1) == 0);
	checkSlow(GpuTier(TEXT("ANGLE (Intel, Intel(R) UHD Graphics 630 Direct3D11 vs_5_0 ps_5_0, D3D11)"),1) == 1);
	checkSlow(GpuTier(TEXT("Adreno (TM) 618"),1) == 2 && GpuTier(TEXT("Adreno (TM) 740"),1) == 0 && GpuTier(TEXT("Mali-G52 MC2"),1) == 2);
	checkSlow(GpuTier(TEXT("llvmpipe (LLVM 15.0.7, 256 bits)"),1) == 3 && GpuTier(TEXT("Mali-G715"),0) == 2);

	INT Tier = GpuTier(appFromAnsi((const ANSICHAR*)glGetString(GL_RENDERER)),SupportsFloatTargets && MaxTextureSize >= 4096);
	if( Tier >= 1 )
		VolumetricFog = GlobalIllumination = 0;
	if( Tier >= 2 )
	{
		ScreenSpaceReflections = AmbientOcclusion = ContactShadows = DepthOfField = 0;
		ResolutionScale = Min(ResolutionScale,75);
	}
	if( Tier >= 3 )
	{
		Antialiasing = HeatHaze = 0;
		ResolutionScale = Min(ResolutionScale,50);
	}
	debugf(TEXT("OpenGLDrv: first run, GPU tier %i"),Tier);
	DetectSettings = 0;
	SaveConfig();
}

UBOOL UOpenGLRenderDevice::Exec(const TCHAR* Cmd,FOutputDevice& Ar)
{
	if( ContextReady && ParseCommand(&Cmd,TEXT("RESETRENDEROPTIONS")) )
	{
		// Default.ini holds the platform's defaults, the packers adjust it for phones and the web.
		// The menu that resets has no controls for these two, so they stay.
		UBOOL VSync = UseVSync, MouseLag = ReduceMouseLag;
		LoadConfig(0,NULL,TEXT("Default.ini"));
		UseVSync		= VSync;
		ReduceMouseLag	= MouseLag;
		DetectOptions();
		return 1;
	}
	return 0;
}

UBOOL UOpenGLRenderDevice::Init()
{
	guard(UOpenGLRenderDevice::Init);

	// The shaders give the engine D3D conventions, see OpenGLVertexShader.
	GIsOpenGL = 0;

	SupportsCubemaps				= 1;
	SupportsZBIAS					= 1;
	SupportsRenderToTextureRGBA8888	= 1;
	SupportsRenderToTextureRGB565	= 0;
	SupportsRenderToTextureNPOT		= 1;
	return 1;

	unguard;
}

static UBOOL HasExtension(const char* Name)
{
	GLint Count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS,&Count);
	for( GLint i=0; i<Count; i++ )
		if( strcmp((const char*)glGetStringi(GL_EXTENSIONS,i),Name) == 0 )
			return 1;
	return 0;
}

static void LogShaderError(GLuint Shader)
{
	ANSICHAR Log[4096] = "";
	glGetShaderInfoLog(Shader,sizeof(Log),NULL,Log);
	debugf(NAME_Warning,TEXT("OpenGLDrv: Shader compile failed: %s"),appFromAnsi(Log));
	GLint Length = 0;
	glGetShaderiv(Shader,GL_SHADER_SOURCE_LENGTH,&Length);
	TArray<ANSICHAR> Source;
	Source.AddZeroed(Length + 1);
	glGetShaderSource(Shader,Length + 1,NULL,&Source(0));
	// Logged line by line, the log truncates long lines.
	FString Text(&Source(0));
	for( INT Pos = Text.InStr(TEXT("\n")); Pos >= 0; Pos = Text.InStr(TEXT("\n")) )
	{
		debugf(NAME_Warning,TEXT("    %s"),*Text.Left(Pos));
		Text = Text.Mid(Pos + 1);
	}
}

static GLuint CompileShader(GLenum Type,const ANSICHAR* Source)
{
	GLuint Shader = glCreateShader(Type);
	glShaderSource(Shader,1,&Source,NULL);
	glCompileShader(Shader);
	GLint Status = 0;
	glGetShaderiv(Shader,GL_COMPILE_STATUS,&Status);
	if( !Status )
	{
		LogShaderError(Shader);
		glDeleteShader(Shader);
		return 0;
	}
	return Shader;
}

GLuint UOpenGLRenderDevice::StartProgram(GLuint VertexShader,const ANSICHAR* FragmentSource,GLuint& FragmentShader)
{
	static const ANSICHAR* Attributes[GLA_Max] = { "aPosition", "aNormal", "aDiffuse", "aSpecular",
		"aTexCoord0", "aTexCoord1", "aTexCoord2", "aTexCoord3", "aTexCoord4", "aTexCoord5", "aTexCoord6", "aTexCoord7" };
	FragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(FragmentShader,1,&FragmentSource,NULL);
	glCompileShader(FragmentShader);
	GLuint Program = glCreateProgram();
	glAttachShader(Program,VertexShader);
	glAttachShader(Program,FragmentShader);
	for( INT i=0; i<GLA_Max; i++ )
		glBindAttribLocation(Program,i,Attributes[i]);
	// Shaders stay attached, ANGLE would wait for the link to detach them.
	glLinkProgram(Program);
	return Program;
}

GLuint UOpenGLRenderDevice::FinishProgram(GLuint Program,GLuint FragmentShader)
{
	GLint Status = 0;
	glGetProgramiv(Program,GL_LINK_STATUS,&Status);
	if( Status )
	{
		GLuint Block = glGetUniformBlockIndex(Program,"Scene");
		if( Block != GL_INVALID_INDEX )
			glUniformBlockBinding(Program,Block,0);
	}
	else
	{
		glGetShaderiv(FragmentShader,GL_COMPILE_STATUS,&Status);
		if( !Status )
			LogShaderError(FragmentShader);
		else
		{
			ANSICHAR Log[4096] = "";
			glGetProgramInfoLog(Program,sizeof(Log),NULL,Log);
			debugf(NAME_Warning,TEXT("OpenGLDrv: Program link failed: %s"),appFromAnsi(Log));
		}
		glDeleteProgram(Program);
		Program = 0;
	}
	glDeleteShader(FragmentShader);
	return Program;
}

GLuint UOpenGLRenderDevice::CompileProgram(GLuint VertexShader,const ANSICHAR* FragmentSource)
{
	if( !VertexShader )
		return 0;
	GLuint FragmentShader;
	GLuint Program = StartProgram(VertexShader,FragmentSource,FragmentShader);
	return FinishProgram(Program,FragmentShader);
}

GLuint UOpenGLRenderDevice::GetSampler(DWORD Flags)
{
	if( GLuint* Found = Samplers.Find(Flags) )
		return *Found;

	GLuint Sampler;
	glGenSamplers(1,&Sampler);
	glSamplerParameteri(Sampler,GL_TEXTURE_WRAP_S,(Flags & GSF_ClampU) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glSamplerParameteri(Sampler,GL_TEXTURE_WRAP_T,(Flags & GSF_ClampV) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glSamplerParameteri(Sampler,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
	glSamplerParameteri(Sampler,GL_TEXTURE_MAG_FILTER,(Flags & GSF_Nearest) ? GL_NEAREST : GL_LINEAR);
	glSamplerParameteri(Sampler,GL_TEXTURE_MIN_FILTER,(Flags & GSF_Nearest) ? GL_NEAREST : (Flags & GSF_Mipmapped) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	if( MaxAnisotropy > 1 && !(Flags & GSF_Nearest) )
		glSamplerParameterf(Sampler,GL_TEXTURE_MAX_ANISOTROPY_EXT,(FLOAT)MaxAnisotropy);
	Samplers.Set(Flags,Sampler);
	return Sampler;
}

// The state of a fresh context, with the object bindings forced to rebind.
void UOpenGLRenderDevice::ResetStateCache()
{
	appMemzero(&GL,sizeof(GL));
	GL.Framebuffer	= (GLuint)-1;
	GL.Program		= (GLuint)-1;
	GL.ElementBuffer= (GLuint)-1;
	GL.DepthFunc	= GL_LESS;
	GL.DepthMask	= GL_TRUE;
	GL.AlphaMask	= GL_TRUE;
	GL.DepthNear	= 0.f;
	GL.DepthFar		= 1.f;
	GL.CullFace		= GL_BACK;
	GL.SrcBlend		= GL_ONE;
	GL.DstBlend		= GL_ZERO;
	GL.NumStreams	= INDEX_NONE;
	for( INT i=0; i<4; i++ )
		GL.Viewport[i] = -1;
	appMemset(GL.Textures,0xff,sizeof(GL.Textures));
	appMemset(GL.Samplers,0xff,sizeof(GL.Samplers));
}

#ifdef SDL_PLATFORM_IOS
// The framebuffer SDL's view drew into when the context was made, in case the window property lacks it.
static GLint GIOSViewFramebuffer = 0;
static INT GIOSFramesLogged = 0;
#endif

UBOOL UOpenGLRenderDevice::InitContext()
{
#ifdef SDL_PLATFORM_IOS
	// Nothing of the engine's is bound yet, so this is SDL's.
	glGetIntegerv(GL_FRAMEBUFFER_BINDING,&GIOSViewFramebuffer);
	GIOSFramesLogged = 0;
	debugf(TEXT("iOS: view framebuffer %i, window property %i"),GIOSViewFramebuffer,
		(INT)SDL_GetNumberProperty(SDL_GetWindowProperties(SDL_GL_GetCurrentWindow()),SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER,-1));
#endif
	debugf(TEXT("GL_VENDOR     : %s"),appFromAnsi((const ANSICHAR*)glGetString(GL_VENDOR)));
	debugf(TEXT("GL_RENDERER   : %s"),appFromAnsi((const ANSICHAR*)glGetString(GL_RENDERER)));
	debugf(TEXT("GL_VERSION    : %s"),appFromAnsi((const ANSICHAR*)glGetString(GL_VERSION)));

	UBOOL S3TC			= HasExtension("GL_EXT_texture_compression_s3tc") || HasExtension("GL_WEBGL_compressed_texture_s3tc");
	SupportsDXT1		= S3TC || HasExtension("GL_EXT_texture_compression_dxt1");
	SupportsDXT35		= S3TC || (HasExtension("GL_ANGLE_texture_compression_dxt3") && HasExtension("GL_ANGLE_texture_compression_dxt5"));
	SupportsFloatTargets = HasExtension("GL_EXT_color_buffer_float");
	glGetIntegerv(GL_MAX_TEXTURE_SIZE,&MaxTextureSize);
	MaxAnisotropy = 1;
	if( HasExtension("GL_EXT_texture_filter_anisotropic") )
	{
		GLfloat Max = 1.f;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&Max);
		MaxAnisotropy = appFloor(Max);
	}
	glGetIntegerv(GL_MAX_SAMPLES,&MaxSamples);
	// Without it every base vertex change respecifies the attributes.
	DrawElementsBaseVertex = NULL;
	if( HasExtension("GL_OES_draw_elements_base_vertex") )
		DrawElementsBaseVertex = (PFNGLDRAWELEMENTSBASEVERTEXOESPROC)SDL_GL_GetProcAddress("glDrawElementsBaseVertexOES");
	else if( HasExtension("GL_EXT_draw_elements_base_vertex") )
		DrawElementsBaseVertex = (PFNGLDRAWELEMENTSBASEVERTEXOESPROC)SDL_GL_GetProcAddress("glDrawElementsBaseVertexEXT");
	ParallelCompile = HasExtension("GL_KHR_parallel_shader_compile");
	if( ParallelCompile )
	{
		// WebGL's version of the extension has no such function, its browsers compile in parallel on their own.
		PFNGLMAXSHADERCOMPILERTHREADSKHRPROC MaxShaderCompilerThreads = (PFNGLMAXSHADERCOMPILERTHREADSKHRPROC)SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR");
		if( MaxShaderCompilerThreads )
			MaxShaderCompilerThreads(0xFFFFFFFF);
	}
	debugf(TEXT("OpenGLDrv: DXT1 %i, DXT3/5 %i, anisotropy %i, max texture %i, base vertex %i, parallel compile %i"),SupportsDXT1,SupportsDXT35,MaxAnisotropy,MaxTextureSize,DrawElementsBaseVertex != NULL,ParallelCompile);

	VertexShaderObject = CompileShader(GL_VERTEX_SHADER,OpenGLVertexShader);
	GLuint FullscreenVertex = CompileShader(GL_VERTEX_SHADER,FullscreenVertexShader);
	UBOOL PostReady = InitPostProcess(FullscreenVertex);
	glDeleteShader(FullscreenVertex);
	if( !VertexShaderObject || !PostReady )
		return 0;

	glGenVertexArrays(1,&EmptyVertexArray);
	glGenVertexArrays(1,&VertexArray);
	glBindVertexArray(VertexArray);
	glGenBuffers(1,&SceneBuffer);
	glBindBuffer(GL_UNIFORM_BUFFER,SceneBuffer);
	glBufferData(GL_UNIFORM_BUFFER,sizeof(FOpenGLSceneBlock),NULL,GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER,0,SceneBuffer);

	glPixelStorei(GL_UNPACK_ALIGNMENT,1);
	glPixelStorei(GL_PACK_ALIGNMENT,1);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_SCISSOR_TEST);
	// Attributes a stream lacks read these, D3D treats missing vertex colours as white.
	glVertexAttrib4f(GLA_Diffuse,1.f,1.f,1.f,1.f);
	glVertexAttrib4f(GLA_Specular,1.f,1.f,1.f,1.f);

	ResetStateCache();
	ContextReady = 1;
	if( DetectSettings )
		DetectOptions();
	// The level may have loaded before this context existed, so its Flush never asked for a precache.
	PrecacheOnFlip = 1;
	return 1;
}

void UOpenGLRenderDevice::CreateFramebuffer(FOpenGLFramebuffer& F,INT Width,INT Height,INT Samples,GLenum Format,UBOOL HasDepth,INT Extra)
{
	if( F.Framebuffer && Width == F.Width && Height == F.Height && Samples == F.Samples )
		return;

	DestroyFramebuffer(F);
	F.Width		= Width;
	F.Height	= Height;
	F.Samples	= Samples;

	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1,&F.Texture);
	glBindTexture(GL_TEXTURE_2D,F.Texture);
	glTexStorage2D(GL_TEXTURE_2D,1,Format,Width,Height);
	glGenFramebuffers(1,&F.Framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER,F.Framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,F.Texture,0);
	if( HasDepth )
	{
		glGenTextures(1,&F.DepthTexture);
		glBindTexture(GL_TEXTURE_2D,F.DepthTexture);
		glTexStorage2D(GL_TEXTURE_2D,1,GL_DEPTH24_STENCIL8,Width,Height);
		glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,F.DepthTexture,0);
	}
	for( INT i=0; i<Extra; i++ )
	{
		glGenTextures(1,&F.ExtraTextures[i]);
		glBindTexture(GL_TEXTURE_2D,F.ExtraTextures[i]);
		glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,Width,Height);
		glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1 + i,GL_TEXTURE_2D,F.ExtraTextures[i],0);
	}
	GL.Textures[0] = (GLuint)-1;

	if( Samples )
	{
		glGenRenderbuffers(1,&F.MultisampleColor);
		glBindRenderbuffer(GL_RENDERBUFFER,F.MultisampleColor);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER,Samples,Format,Width,Height);
		glGenFramebuffers(1,&F.MultisampleFramebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER,F.MultisampleFramebuffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,F.MultisampleColor);
		if( HasDepth )
		{
			glGenRenderbuffers(1,&F.MultisampleDepth);
			glBindRenderbuffer(GL_RENDERBUFFER,F.MultisampleDepth);
			glRenderbufferStorageMultisample(GL_RENDERBUFFER,Samples,GL_DEPTH24_STENCIL8,Width,Height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,F.MultisampleDepth);
		}
		for( INT i=0; i<Extra; i++ )
		{
			glGenRenderbuffers(1,&F.MultisampleExtra[i]);
			glBindRenderbuffer(GL_RENDERBUFFER,F.MultisampleExtra[i]);
			glRenderbufferStorageMultisample(GL_RENDERBUFFER,Samples,GL_RGBA8,Width,Height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1 + i,GL_RENDERBUFFER,F.MultisampleExtra[i]);
		}
	}

	if( glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE )
		debugf(NAME_Warning,TEXT("OpenGLDrv: Incomplete framebuffer %ix%i, %i samples"),Width,Height,Samples);
	GL.Framebuffer = F.DrawFramebuffer();
}

void UOpenGLRenderDevice::DestroyFramebuffer(FOpenGLFramebuffer& F)
{
	if( !F.Framebuffer )
		return;
	// Zero names are ignored.
	GLuint	Framebuffers[] = { F.Framebuffer, F.MultisampleFramebuffer },
			Textures[] = { F.Texture, F.DepthTexture, F.ExtraTextures[0], F.ExtraTextures[1] },
			Renderbuffers[] = { F.MultisampleColor, F.MultisampleDepth, F.MultisampleExtra[0], F.MultisampleExtra[1] };
	glDeleteFramebuffers(ARRAY_COUNT(Framebuffers),Framebuffers);
	glDeleteTextures(ARRAY_COUNT(Textures),Textures);
	glDeleteRenderbuffers(ARRAY_COUNT(Renderbuffers),Renderbuffers);
	F = FOpenGLFramebuffer();
	GL.Framebuffer = (GLuint)-1;
	for( INT i=0; i<OGL_MAX_TEXTURES; i++ )
		GL.Textures[i] = (GLuint)-1;
}

UBOOL UOpenGLRenderDevice::SetRes(UViewport* Viewport,INT NewX,INT NewY,UBOOL Fullscreen,INT ColorBytes,UBOOL bSaveSize)
{
	guard(UOpenGLRenderDevice::SetRes);

	if( LockedViewport )
		appErrorf(TEXT("Can't change resolution while render device is locked!"));
	if( appStricmp(Viewport->GetClass()->GetName(),TEXT("SDLViewport")) != 0 )
	{
		debugf(TEXT("OpenGLDrv: Needs the SDL viewport of ut2004-bin"));
		return 0;
	}

	// On Windows this makes SDL use ANGLE's EGL instead of a native ES context from the driver.
	SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER,"1");
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE,8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,8);
	// The scene framebuffer has the depth and stencil, the window only receives the final copy.
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,0);

	if( !Viewport->ResizeViewport((Fullscreen ? BLIT_Fullscreen : 0) | BLIT_OpenGL,NewX,NewY,bSaveSize) )
		return 0;

	// SDLDrv keeps the first context it made, which is a desktop one after OpenGLDrv ran.
	const ANSICHAR* Version = (const ANSICHAR*)glGetString(GL_VERSION);
	if( !Version || strncmp(Version,"OpenGL ES 3",11) != 0 )
	{
		debugf(TEXT("OpenGLDrv: No OpenGL ES 3 context (%s)"),Version ? appFromAnsi(Version) : TEXT("none"));
		return 0;
	}
	if( !ContextReady && !InitContext() )
		return 0;

	SDL_GL_SetSwapInterval(UseVSync ? 1 : 0);
	Viewport->PendingFrame = 0;
	return 1;

	unguard;
}

void UOpenGLRenderDevice::ReleaseObjects()
{
	for( TMap<DWORD,GLuint>::TIterator It(Samplers); It; ++It )
		glDeleteSamplers(1,&It.Value());
	Samplers.Empty();
	appMemset(GL.Samplers,0xff,sizeof(GL.Samplers));
	DestroyFramebuffer(Scene);
	DestroyFramebuffer(World);
	DestroyFramebuffer(Occlusion);
	DestroyFramebuffer(Focus);
	DestroyFramebuffer(Scattering);
	DestroyFramebuffer(ScatterHistory[0]);
	DestroyFramebuffer(ScatterHistory[1]);
	DestroyFramebuffer(Indirect);
	DestroyFramebuffer(IndirectHistory[0]);
	DestroyFramebuffer(IndirectHistory[1]);
	DestroyFramebuffer(Overlay);
	DynamicVertexBuffer.Release();
	DynamicIndexBuffer.Release();
	if( QuadIndices )			glDeleteBuffers(1,&QuadIndices);
	if( SceneBuffer )			glDeleteBuffers(1,&SceneBuffer);
	if( VertexArray )			glDeleteVertexArrays(1,&VertexArray);
	if( EmptyVertexArray )		glDeleteVertexArrays(1,&EmptyVertexArray);
	GLuint PostPrograms[] = { CopyProgram, OcclusionProgram, CompositeProgram, DepthOfFieldProgram, VolumetricProgram, ResolveProgram, OverlayProgram, IndirectProgram };
	for( INT i=0; i<ARRAY_COUNT(PostPrograms); i++ )
		glDeleteProgram(PostPrograms[i]);
	if( VertexShaderObject )	glDeleteShader(VertexShaderObject);
	if( FrameFence )			glDeleteSync(FrameFence);
	GLuint ShadowTextures[] = { ShadowAtlas, SunShadow };
	glDeleteTextures(ARRAY_COUNT(ShadowTextures),ShadowTextures);
	FlushShadows();
	FrameFence = NULL;
	VertexArray = EmptyVertexArray = SceneBuffer = QuadIndices = VertexShaderObject = 0;
	CopyProgram = OcclusionProgram = CompositeProgram = DepthOfFieldProgram = VolumetricProgram = ResolveProgram = OverlayProgram = IndirectProgram = ShadowAtlas = SunShadow = 0;
}

void UOpenGLRenderDevice::Exit(UViewport* Viewport)
{
	guard(UOpenGLRenderDevice::Exit);
	if( ContextReady )
	{
		Flush(Viewport);
		ReleaseObjects();
		ContextReady = 0;
	}
	unguard;
}

void UOpenGLRenderDevice::Flush(UViewport* Viewport)
{
	guard(UOpenGLRenderDevice::Flush);

	TArray<FOpenGLResource*> All;
	for( TMap<QWORD,FOpenGLResource*>::TIterator It(Resources); It; ++It )
		All.AddItem(It.Value());
	for( INT i=0; i<All.Num(); i++ )
		delete All(i);

	for( TMultiMap<DWORD,FOpenGLProgram*>::TIterator It(Programs); It; ++It )
	{
		if( It.Value()->Program )
			glDeleteProgram(It.Value()->Program);
		if( It.Value()->Pending )
			glDeleteShader(It.Value()->FragmentShader);
		delete It.Value();
	}
	Programs.Empty();
	GL.Program = (GLuint)-1;
	GL.NumStreams = INDEX_NONE;

	// The base state outlives frames, drop what it points at.
	FOpenGLRenderInterface::FSavedState& Base = RenderInterface.SavedStates[0];
	Base.Material.Reset();
	Base.RenderTarget			= NULL;
	Base.Lightmap				= NULL;
	Base.NumStreams				= 0;
	Base.Source					= NULL;
	Base.IndexBuffer			= 0;

	PrecacheOnFlip = 1;

	unguard;
}

FOpenGLResource* UOpenGLRenderDevice::GetCachedResource(QWORD CacheId)
{
	FOpenGLResource** Found = Resources.Find(CacheId);
	return Found ? *Found : NULL;
}

void UOpenGLRenderDevice::FlushResource(QWORD CacheId)
{
	delete GetCachedResource(CacheId);
}

UBOOL UOpenGLRenderDevice::ResourceCached(QWORD CacheId)
{
	return GetCachedResource(CacheId) != NULL;
}

FRenderInterface* UOpenGLRenderDevice::Lock(UViewport* Viewport,BYTE* HitData,INT* HitSize)
{
	guard(UOpenGLRenderDevice::Lock);

	// A frame that draws no world, like a loading screen, leaves the previous view outdated.
	PrevViewValid	= FrameViewValid;
	FrameViewValid	= 0;

	// Window resizes and the menu's set ini reach the device without SetRes.
	INT Samples = Antialiasing ? Min<INT>(4,MaxSamples) : 0;
	CreateFramebuffer(Scene,Viewport->SizeX,Viewport->SizeY,Samples);
	INT Scale = Clamp(ResolutionScale,50,100);
	CreateFramebuffer(World,(Viewport->SizeX * Scale + 99) / 100,(Viewport->SizeY * Scale + 99) / 100,Samples,GL_RGBA8,1,2);
	glBindFramebuffer(GL_FRAMEBUFFER,World.DrawFramebuffer());
	SetWorldDrawBuffers(0);
	GL.Framebuffer = World.DrawFramebuffer();
	LockedViewport = Viewport;
	Scene.Resolved = World.Resolved = 0;
	RenderInterface.Locked(Viewport);
	return &RenderInterface;

	unguard;
}

void UOpenGLRenderDevice::Unlock(FRenderInterface* RI)
{
	RenderInterface.Unlocked();
	// The loading screen covers the compiles the precache started, so the first frame has the whole level.
	if( LockedViewport && LockedViewport->Precaching )
		for( TMultiMap<DWORD,FOpenGLProgram*>::TIterator It(Programs); It; ++It )
			ProgramReady(It.Value(),1);
	LockedViewport = NULL;
}

void UOpenGLRenderDevice::ResolveFramebuffer(FOpenGLFramebuffer& F,UBOOL Depth)
{
	if( F.Samples && !F.Resolved )
	{
		glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER,F.MultisampleFramebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER,F.Framebuffer);
		glBlitFramebuffer(0,0,F.Width,F.Height,0,0,F.Width,F.Height,GL_COLOR_BUFFER_BIT | (Depth ? GL_DEPTH_BUFFER_BIT : 0),GL_NEAREST);
		// A blit reads one colour attachment and writes every draw buffer.
		INT i;
		for( i=0; i<ARRAY_COUNT(F.ExtraTextures) && F.ExtraTextures[i]; i++ )
		{
			GLenum Only[] = { GL_NONE, GL_NONE, GL_NONE }, First = GL_COLOR_ATTACHMENT0;
			Only[i + 1] = GL_COLOR_ATTACHMENT1 + i;
			glReadBuffer(GL_COLOR_ATTACHMENT1 + i);
			glDrawBuffers(i + 2,Only);
			glBlitFramebuffer(0,0,F.Width,F.Height,0,0,F.Width,F.Height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glDrawBuffers(1,&First);
		}
		glEnable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER,F.MultisampleFramebuffer);
		GLenum Attachments[] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glInvalidateFramebuffer(GL_FRAMEBUFFER,2 + i,Attachments);
		GL.Framebuffer = F.MultisampleFramebuffer;
	}
	F.Resolved = 1;
}

void UOpenGLRenderDevice::SetWorldDrawBuffers(UBOOL ReflectionOnly)
{
	GLenum Buffers[] =
	{
		ReflectionOnly ? (GLenum)GL_NONE : GL_COLOR_ATTACHMENT0,
		ScreenSpaceReflections ? GL_COLOR_ATTACHMENT1 : (GLenum)GL_NONE,
		GlobalIllumination && !ReflectionOnly ? GL_COLOR_ATTACHMENT2 : (GLenum)GL_NONE,
	};
	glDrawBuffers(ARRAY_COUNT(Buffers),Buffers);
}

void UOpenGLRenderDevice::SetAlphaMask(GLboolean Alpha)
{
	if( GL.AlphaMask != Alpha )
	{
		glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,Alpha);
		GL.AlphaMask = Alpha;
	}
}

void UOpenGLRenderDevice::UseProgram(GLuint Program)
{
	if( GL.Program != Program )
	{
		glUseProgram(Program);
		GL.Program = Program;
	}
}

// ES 3 cannot scale a blit into a multisampled target, a draw can.
void UOpenGLRenderDevice::DrawFullscreen(GLuint Program,GLuint Framebuffer,INT Width,INT Height,GLuint Texture0,GLuint Texture1,GLuint Depth,UBOOL Over,GLuint Texture3,GLuint Texture4,GLuint Texture5,GLuint Texture6)
{
	glBindFramebuffer(GL_FRAMEBUFFER,Framebuffer);
	glViewport(0,0,Width,Height);
	SetAlphaMask(GL_TRUE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	if( Over )
	{
		glEnable(GL_BLEND);
		GL.SrcBlend = GL_ONE;
		GL.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
		glBlendFunc(GL.SrcBlend,GL.DstBlend);
	}
	else
		glDisable(GL_BLEND);
	UseProgram(Program);
	GLuint Textures[] = { Texture0, Texture1, Depth, Texture3, Texture4, Texture5, Texture6 };
	for( INT i=0; i<ARRAY_COUNT(Textures); i++ )
	{
		if( !Textures[i] )
			continue;
		// R32F is incomplete with linear filtering unless OES_texture_float_linear is there, which many phones and browsers lack.
		// The sun map is only read with texelFetch, so nearest costs nothing.
		UBOOL Nearest = i == 2 || Textures[i] == SunShadow;
		GLuint Sampler = GetSampler(GSF_ClampU | GSF_ClampV | (Nearest ? GSF_Nearest : 0));
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D,Textures[i]);
		glBindSampler(i,Sampler);
		GL.Textures[i]			= Textures[i];
		GL.TextureTargets[i]	= GL_TEXTURE_2D;
		GL.Samplers[i]			= Sampler;
	}
	// Attribute free, so the scene's enabled arrays stay out of it.
	glBindVertexArray(EmptyVertexArray);
	glDrawArrays(GL_TRIANGLES,0,3);
	glBindVertexArray(VertexArray);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_SCISSOR_TEST);

	GL.Framebuffer	= Framebuffer;
	GL.Blend		= Over;
	GL.Cull			= 0;
	appMemset(GL.Viewport,0xff,sizeof(GL.Viewport));
}

void UOpenGLRenderDevice::Present(UViewport* Viewport)
{
	guard(UOpenGLRenderDevice::Present);

	SDL_Window* Window = SDL_GL_GetCurrentWindow();
	if( !Scene.Framebuffer || !Window )
		return;
	ResolveFramebuffer(Scene);

	int Width = Scene.Width, Height = Scene.Height;
	SDL_GetWindowSizeInPixels(Window,&Width,&Height);
	// On iOS the window is a framebuffer object SDL made, there is no default one.
	GLuint WindowFramebuffer = 0;
#ifdef SDL_PLATFORM_IOS
	WindowFramebuffer = (GLuint)SDL_GetNumberProperty(SDL_GetWindowProperties(Window),SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER,0);
	if( !WindowFramebuffer )
		WindowFramebuffer = (GLuint)GIOSViewFramebuffer;
#endif
	// Depth is not needed past the frame and the copy covers the whole window.
	GLenum DepthStencil = GL_DEPTH_STENCIL_ATTACHMENT, Color = WindowFramebuffer ? GL_COLOR_ATTACHMENT0 : GL_COLOR;
	glBindFramebuffer(GL_FRAMEBUFFER,Scene.Framebuffer);
	glInvalidateFramebuffer(GL_FRAMEBUFFER,1,&DepthStencil);
	glBindFramebuffer(GL_FRAMEBUFFER,WindowFramebuffer);
	glInvalidateFramebuffer(GL_FRAMEBUFFER,1,&Color);
	DrawFullscreen(CopyProgram,WindowFramebuffer,Width,Height,Scene.Texture);

#ifdef SDL_PLATFORM_IOS
	if( GIOSFramesLogged < 3 )
	{
		GIOSFramesLogged++;
		glBindFramebuffer(GL_FRAMEBUFFER,WindowFramebuffer);
		GLint RenderbufferWidth = 0, RenderbufferHeight = 0;
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&RenderbufferWidth);
		GLint Type = RenderbufferWidth;
		RenderbufferWidth = 0;
		GLint Renderbuffer = (GLint)SDL_GetNumberProperty(SDL_GetWindowProperties(Window),SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER,0);
		if( Renderbuffer )
		{
			glBindRenderbuffer(GL_RENDERBUFFER,Renderbuffer);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_WIDTH,&RenderbufferWidth);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_HEIGHT,&RenderbufferHeight);
		}
		int PointsX = 0, PointsY = 0;
		SDL_GetWindowSize(Window,&PointsX,&PointsY);
		debugf(TEXT("iOS: present %i into framebuffer %i (status 0x%x, attachment type 0x%x, renderbuffer %i at %ix%i), window %ix%i pixels %ix%i points, scene %ix%i, flags 0x%llx, GL error 0x%x"),
			GIOSFramesLogged,WindowFramebuffer,glCheckFramebufferStatus(GL_FRAMEBUFFER),Type,Renderbuffer,RenderbufferWidth,RenderbufferHeight,
			Width,Height,PointsX,PointsY,Scene.Width,Scene.Height,(unsigned long long)SDL_GetWindowFlags(Window),glGetError());
		GL.Framebuffer = WindowFramebuffer;
	}
#endif

	SDL_GL_SwapWindow(Window);

	// Waiting for the previous frame keeps the driver from queueing more, each queued frame is input lag.
	if( FrameFence )
	{
		glClientWaitSync(FrameFence,GL_SYNC_FLUSH_COMMANDS_BIT,100000000);
		glDeleteSync(FrameFence);
		FrameFence = NULL;
	}
#ifndef __EMSCRIPTEN__
	// WebGL caps the wait at MAX_CLIENT_WAIT_TIMEOUT_WEBGL, usually 0, and the browser only presents once the frame returns.
	if( ReduceMouseLag )
		FrameFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
#endif

	unguard;
}

void UOpenGLRenderDevice::ReadPixels(UViewport* Viewport,FColor* Pixels,UBOOL Flipped)
{
	guard(UOpenGLRenderDevice::ReadPixels);

	if( !Viewport || !Pixels || !Scene.Framebuffer )
		return;
	ResolveFramebuffer(Scene);
	glBindFramebuffer(GL_FRAMEBUFFER,Scene.Framebuffer);
	GL.Framebuffer = Scene.Framebuffer;

	INT Width = Min(Viewport->SizeX,Scene.Width), Height = Min(Viewport->SizeY,Scene.Height);
	glReadPixels(0,0,Width,Height,GL_RGBA,GL_UNSIGNED_BYTE,Pixels);
	for( INT i=0; i<Width*Height; i++ )
		Exchange(Pixels[i].R,Pixels[i].B);
	// GL rows run bottom up.
	if( !Flipped )
		for( INT Y=0; Y<Height/2; Y++ )
			for( INT X=0; X<Width; X++ )
				Exchange(Pixels[Y * Width + X],Pixels[(Height - 1 - Y) * Width + X]);

	unguard;
}

FRenderCaps* UOpenGLRenderDevice::GetRenderCaps()
{
	RenderCaps.HardwareTL					= 1;
	RenderCaps.PixelShaderVersion			= 14;
	RenderCaps.MaxSimultaneousTerrainLayers	= 4;
	return &RenderCaps;
}
