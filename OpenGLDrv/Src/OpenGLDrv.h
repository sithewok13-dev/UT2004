#ifndef HEADER_OPENGLDRV
#define HEADER_OPENGLDRV

// SDL3.dll uses default packing, the engine builds with /Zp4.
#pragma pack(push,8)
#include <SDL3/SDL.h>
#pragma pack(pop)
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "Engine.h"

class UOpenGLRenderDevice;

// The first person weapon is drawn into the front of the depth range.
#define VIEW_MODEL_DEPTH 0.015625

enum
{
	OGL_MAX_TEXTURES	= 16,
	OGL_MAX_MATRICES	= 8,
	OGL_MAX_CONSTANTS	= 8,
	OGL_MAX_LIGHTS		= 8,
	OGL_MAX_STREAMS		= 8,
	OGL_MAX_QUADS		= 5461,
};

// Defines, so the volumetric fog shader can be built from them.
#define FOG_LIGHTS		16
#define FOG_TILE		32	// Octahedral distance map of a light, in texels.
#define FOG_ATLAS_TILES	16	// Tiles across the atlas.
#define FOG_SUN_SIZE	64

// The share of the distance fog at X from its start to its end. It builds up exponentially like real
// fog and ramps in after the start so there is no visible edge, yet still covers everything at the
// end, where the level is culled. FogAt is the share at camera space P, for the scene block's Fog.
// Volumetric fog is thinner upward and thicker downward.
#define OGL_FOG_AMOUNT \
	"float FogAmount(float X, float Density)\n" \
	"{\n" \
	"	float Depth = X < 0.3 ? 5.0 * X * X : 3.0 * X - 0.45;\n" \
	"	return (1.0 - exp(-Density * Depth)) / (1.0 - exp(-Density * 2.55));\n" \
	"}\n" \
	"float FogAt(vec3 P, vec3 Up, vec4 Fog)\n" \
	"{\n" \
	"	if (Fog.z < 0.5)\n" \
	"		return 0.0;\n" \
	"	float Dist = length(P);\n" \
	"	float X = clamp((Dist - Fog.x) / (Fog.y - Fog.x), 0.0, 1.0);\n" \
	"	return Fog.w > 0.5 ? FogAmount(X, exp2(-1.5 * dot(P, Up) / max(Dist, 1.0))) : X;\n" \
	"}\n"

enum EOpenGLAttribute
{
	GLA_Position,
	GLA_Normal,
	GLA_Diffuse,
	GLA_Specular,
	GLA_TexCoord0,
	GLA_Max = GLA_TexCoord0 + 8
};

class FOpenGLResource
{
public:
	UOpenGLRenderDevice*	RenDev;
	QWORD				CacheId;
	INT					CachedRevision;

	FOpenGLResource(UOpenGLRenderDevice* InRenDev,QWORD InCacheId);
	virtual ~FOpenGLResource();
};

class FOpenGLTexture : public FOpenGLResource
{
public:
	GLuint	Texture;
	GLenum	Target;
	GLenum	InternalFormat;
	INT		Width,
			Height,
			NumMips;
	GLuint	Framebuffer,
			DepthStencil;
	TArray<INT>	ChildRevisions;

	FOpenGLTexture(UOpenGLRenderDevice* InRenDev,QWORD InCacheId);
	virtual ~FOpenGLTexture();

	void Cache(FBaseTexture* Source);

private:
	void Release();
	void Allocate(GLenum InTarget,GLenum InFormat,INT InWidth,INT InHeight,INT InNumMips);
	void Upload(GLenum Face,INT Level,FTexture* Source,INT SourceMip,ETextureFormat Format,INT MipWidth,INT MipHeight);
};

class FOpenGLVertexStream : public FOpenGLResource
{
public:
	GLuint	Buffer;
	INT		Size;

	FOpenGLVertexStream(UOpenGLRenderDevice* InRenDev,QWORD InCacheId);
	virtual ~FOpenGLVertexStream();

	void Cache(FVertexStream* Source);
};

class FOpenGLIndexBuffer : public FOpenGLResource
{
public:
	GLuint	Buffer;
	INT		Size;

	FOpenGLIndexBuffer(UOpenGLRenderDevice* InRenDev,QWORD InCacheId);
	virtual ~FOpenGLIndexBuffer();

	void Cache(FIndexBuffer* Source);
};

// A ring buffer for per-frame data, orphaned when full.
struct FOpenGLDynamicBuffer
{
	GLenum	Target;
	GLuint	Buffer;
	INT		Size,
			Used,
			LockedOffset,
			LockedBytes;
	TArray<BYTE>	Scratch;

	FOpenGLDynamicBuffer(GLenum InTarget) : Target(InTarget), Buffer(0), Size(0), Used(0), LockedOffset(0), LockedBytes(0) {}
	// Returns room for Bytes at the next aligned offset, which Offset receives, uploaded by Unlock. NULL for no bytes.
	BYTE* Lock(INT Bytes,INT Alignment,INT& Offset);
	void Unlock();
	void Release();
};

struct FOpenGLProgram
{
	TArray<BYTE>	Key;
	GLuint			Program,
					FragmentShader;	// Kept until the link finishes, for its error log.
	UBOOL			Pending;
	GLint			LocalToWorld,
					NormalToWorld,
					AlphaRef,
					Probe,
					Matrices[OGL_MAX_MATRICES],
					Constants[OGL_MAX_CONSTANTS];
	// Last values sent, uniforms belong to the program so unchanged ones are skipped.
	FMatrix			SentLocalToWorld,
					SentMatrices[OGL_MAX_MATRICES];
	FPlane			SentConstants[OGL_MAX_CONSTANTS];
	FLOAT			SentAlphaRef;
};

enum EOpenGLSamplerFlags
{
	GSF_ClampU		= 1,
	GSF_ClampV		= 2,
	GSF_Mipmapped	= 4,
	GSF_Nearest		= 8,	// Depth textures are incomplete with linear filtering.
};

struct FOpenGLMaterialState
{
	FOpenGLProgram*	Program;
	INT				NumTextures,
					NumMatrices,
					NumConstants;
	FOpenGLTexture*	Textures[OGL_MAX_TEXTURES];
	BYTE			SamplerFlags[OGL_MAX_TEXTURES];
	FMatrix			Matrices[OGL_MAX_MATRICES];
	FPlane			Constants[OGL_MAX_CONSTANTS];
	INT				GlobalColorConstant;	// The constant SetGlobalColor replaces, or INDEX_NONE.
	GLenum			SrcBlend,
					DstBlend;
	UBOOL			AlphaTest;
	FLOAT			AlphaRef;
	UBOOL			ZWrite,
					ZTest,
					TwoSided,
					Beam,			// A fake light shaft, drawn after the rest of the level.
					Heat,			// Writes how hot it looks into the world's alpha, for the heat haze.
					DepthPass,		// Blended without depth, drawn again into depth where mostly opaque, so the post process fog sees it.
					Reflectivity;	// Blends its reflectivity into the reflection's alpha.

	void Reset();
};

struct FOpenGLLight
{
	FPlane	Position;	// W is the radius.
	FPlane	Direction;
	FPlane	Color;
	FPlane	Params;		// X is the type, Y the spot cone sine.
};

// Layout of the std140 Scene block in the shaders.
struct FOpenGLSceneBlock
{
	FMatrix	WorldToCamera,
			CameraToScreen;
	FPlane	ClipAdjust,
			Fog,
			FogColor,
			Ambient,
			Options,		// X dynamic lights, Y static lighting, Z 2x lighting, W mip bias.
			FogUp;			// World up in camera space, for the volumetric fog's height. W is 1 in the world framebuffer.
	FPlane	LightPosition[OGL_MAX_LIGHTS],
			LightDirection[OGL_MAX_LIGHTS],
			LightColor[OGL_MAX_LIGHTS],
			LightParams[OGL_MAX_LIGHTS];
};

// What the volumetric fog pass needs from the CPU. Positions and directions are in camera space.
struct FOpenGLFogLights
{
	INT		NumLights;
	FPlane	Position[FOG_LIGHTS],	// W is the radius.
			Color[FOG_LIGHTS],		// W is the atlas tile, -1 for none.
			Spot[FOG_LIGHTS],		// W is the cone sine, -1 for a point light.
			Sun,					// Toward the sun, W 1 if there is one.
			SunColor,
			SunU,					// The sun map's axes in texels and the boundary axis in units, W their value at the camera.
			SunV,
			SunAxis,
			Fog;					// Start, end, enabled and the scattering density.
};

struct FOpenGLStream
{
	GLuint				Buffer;
	INT					Stride,
						NumComponents;
	FVertexComponent	Components[MAX_VERTEX_COMPONENTS];
};

class FOpenGLRenderInterface : public FRenderInterface
{
public:
	struct FSavedState
	{
		FOpenGLTexture*		RenderTarget;
		INT					ViewportX,
							ViewportY,
							ViewportWidth,
							ViewportHeight;
		INT					ZBias;


		FMatrix				LocalToWorld,
							WorldToCamera,
							CameraToScreen;

		FOpenGLStream			Streams[OGL_MAX_STREAMS];
		INT					NumStreams;
		FVertexStream*		Source;			// What Streams[0] was cached from, NULL for dynamic vertices. Only valid until the draw.
		GLuint				IndexBuffer;
		INT					IndexSize,
							IndexBufferBase;

		ECullMode			CullMode;

		UBOOL				UseDynamicLighting,
							UseStaticLighting,
							LightingModulate2X,
							LightingOnly;
		FOpenGLTexture*		Lightmap;
		FSphere				LitSphere;
		FColor				AmbientLightColor;
		FOpenGLLight			Lights[OGL_MAX_LIGHTS];
		UBOOL				LightEnabled[OGL_MAX_LIGHTS];

		UBOOL				FogEnabled;
		FLOAT				FogStart,
							FogEnd;
		FColor				FogColor;

		FOpenGLMaterialState	Material;
	};

	UOpenGLRenderDevice*	RenDev;
	UViewport*			Viewport;
	enum { MAX_STATE_DEPTH = 128 };
	FSavedState			SavedStates[MAX_STATE_DEPTH];
	FSavedState*		CurrentState;
	INT					SavedStateIndex;
	EPrecacheMode		PrecacheMode;
	UBOOL				InWorld,
						ViewModel,		// Only the first person weapon is left of the world.
						WorldDrawn,		// The level reached the world framebuffer, a motion blur draws it elsewhere.
						OverlayUsed;	// 2D drawn within the world went to the overlay this frame.
	FMatrix				ViewWorldToCamera,
						ViewCameraToScreen;
	TArray<FDynamicLight*>	WorldLights;	// The lights actors in the world were lit by.
	FLOAT				FocusDiopters;
	DOUBLE				FocusTime;
	FPlane				ViewFog,		// The player's zone fog, start, end, enabled and W 1 once recorded.
						ViewFogColor;
	FVector				FogUp;
	FOpenGLFogLights	FogLights;
	TMap<AActor*,FLOAT>	FogFades;
	DOUBLE				FogTime;

	TArray<BYTE>		DynamicVertices;
	INT					DynamicStride,
						DynamicSize;
	DWORD				DynamicComponents;
	FIndexBuffer*		DynamicIndices;		// The last SetDynamicIndexBuffer source, kept to copy deferred draws.
	INT					DynamicIndicesFirst;

	struct FDeferredDraw
	{
		FSavedState		State;
		EPrimitiveType	Type;
		INT				FirstIndex,
						NumPrimitives,
						MinIndex,
						MaxIndex,
						IndexOffset,		// Into DeferredIndices, for draws from the dynamic index buffer.
						IndexBytes;
	};
	TArray<FDeferredDraw>	DeferredDraws;
	TArray<BYTE>			DeferredIndices;

	// A world env map's capture point and the box its surroundings are projected on, the Probe uniform.
	struct FProbe
	{
		FVector	Origin,
				Min,
				Max;
	};
	TMap<QWORD,FProbe>	Probes;			// By quantised origin, so objects that stay put trace once.

	// Explosions seen lately. Actors carry no spawn time, so the first frame one shows up starts its blast wave.
	struct FBlast
	{
		AActor*	Actor;		// Only compared, NULL once the actor is gone.
		FVector	Location;
		FLOAT	Radius,
				Start;
	};
	FBlast				Blasts[16];
	FName				ProbeMap;

	FOpenGLRenderInterface(UOpenGLRenderDevice* InRenDev);
	void DrawDeferred();

	void Locked(UViewport* InViewport);
	void Unlocked();

	// FRenderInterface interface.
	virtual void PushState();
	virtual void PopState();
	virtual UBOOL SetRenderTarget(FRenderTarget* RenderTarget);
	virtual void SetViewport(INT X,INT Y,INT Width,INT Height);
	virtual void Clear(UBOOL UseColor,FColor Color,UBOOL UseDepth,FLOAT Depth,UBOOL UseStencil,DWORD Stencil);
	virtual void BeginWorld();
	virtual void BeginViewModel(const FMatrix& WorldToCamera,const FMatrix& CameraToScreen);
	void EndWorld();
	virtual void PushHit(const BYTE* Data,INT Count) {}
	virtual void PopHit(INT Count,UBOOL Force) {}
	virtual void SetCullMode(ECullMode CullMode);
	virtual void SetAmbientLight(FColor Color);
	virtual void EnableLighting(UBOOL UseDynamic,UBOOL UseStatic,UBOOL Modulate2X,FBaseTexture* Lightmap,UBOOL LightingOnly,FSphere LitSphere);
	virtual void SetLight(INT LightIndex,FDynamicLight* Light,FLOAT Scale);
	virtual void SetNPatchTesselation(FLOAT Tesselation) {}
	virtual void SetDistanceFog(UBOOL Enable,FLOAT FogStart,FLOAT FogEnd,FColor Color);
	virtual void SetGlobalColor(FColor Color);
	virtual void SetTransform(ETransformType Type,const FMatrix& Matrix);
	virtual void SetMaterial(UMaterial* Material,FString* ErrorString,UMaterial** ErrorMaterial,INT* NumPasses);
	// Retail renders without stencil, its portals and sky draw child scenes first.
	virtual void SetStencilOp(ECompareFunction Test,DWORD Ref,DWORD Mask,EStencilOp FailOp,EStencilOp ZFailOp,EStencilOp PassOp,DWORD WriteMask) {}
	virtual void SetPrecacheMode(EPrecacheMode InPrecacheMode) { PrecacheMode = InPrecacheMode; }
	virtual void SetZBias(INT ZBias);
	virtual INT SetVertexStreams(EVertexShader Shader,FVertexStream** Streams,INT NumStreams);
	virtual INT SetDynamicStream(EVertexShader Shader,FVertexStream* Stream);
	virtual INT SetIndexBuffer(FIndexBuffer* IndexBuffer,INT BaseVertexIndex);
	virtual INT SetDynamicIndexBuffer(FIndexBuffer* IndexBuffer,INT BaseVertexIndex);
	virtual void DrawPrimitive(EPrimitiveType PrimitiveType,INT FirstIndex,INT NumPrimitives,INT MinIndex,INT MaxIndex);
	virtual int LockDynBuffer(BYTE** OutBuffer,int NumVerts,int Stride,DWORD ComponentFlags);
	virtual int UnlockDynBuffer();
	virtual void DrawDynQuads(INT NumPrimitives);
	virtual void DrawQuads(INT FirstVertex,INT NumPrimitives);

	FOpenGLTexture* CacheTexture(FBaseTexture* Texture);

private:
	INT UseDynamicVertices(INT Offset,INT Stride,const FVertexComponent* Components,INT NumComponents);
	void SetQuadIndexBuffer(INT BaseVertexIndex);
	void Commit();
	UBOOL CompileMaterial(UMaterial* Material,FString* ErrorString,UMaterial** ErrorMaterial);
	FPlane PickShadowLight();
	FProbe FindProbe(const FMatrix& LocalToWorld);
	FLOAT PickFocus();
	const FOpenGLFogLights* PickFogLights();
	void PickBlasts(FPlane* Out);
	void TraceShadows(ULevel* Level,const FVector& Camera);
	FLOAT TraceSunCell(ULevel* Level,INT I,INT J,FLOAT CameraS);
};

enum EOpenGLExtraTarget { EXTRA_Reflection, EXTRA_Albedo };
enum EOpenGLHeat { HEAT_None, HEAT_Hot, HEAT_Lava };
#define HEAT_BLASTS 4

// A colour texture with optional depth and stencil and extra colour textures, the world's reflection and albedo.
// With multisampling the drawing goes to separate buffers that Resolve copies into the textures.
struct FOpenGLFramebuffer
{
	GLuint	Framebuffer,
			Texture,
			DepthTexture,
			MultisampleFramebuffer,
			MultisampleColor,
			MultisampleDepth,
			ExtraTextures[2],	// Indexed by EOpenGLExtraTarget.
			MultisampleExtra[2];
	INT		Width,
			Height,
			Samples;
	UBOOL	Resolved;

	FOpenGLFramebuffer() { appMemzero(this,sizeof(*this)); }
	GLuint DrawFramebuffer() const { return Samples ? MultisampleFramebuffer : Framebuffer; }
};

class UOpenGLRenderDevice : public URenderDevice
{
	DECLARE_CLASS(UOpenGLRenderDevice,URenderDevice,CLASS_Config,OpenGLDrv)

	UBOOL	UseVSync,
			ReduceMouseLag,
			Antialiasing,
			AmbientOcclusion,
			ContactShadows,
			ScreenSpaceReflections,
			DepthOfField,
			VolumetricFog,
			GlobalIllumination,
			HeatHaze,
			MotionBlur,
			DetectSettings;
	INT		ResolutionScale;	// Percent of the window the world renders at.

	UBOOL	SupportsDXT1,
			SupportsDXT35,
			SupportsFloatTargets,
			ParallelCompile;
	INT		MaxTextureSize,
			MaxAnisotropy,
			MaxSamples;

	FRenderCaps				RenderCaps;
	FOpenGLRenderInterface	RenderInterface;
	UViewport*				LockedViewport;
	UBOOL					ContextReady;

	TMap<QWORD,FOpenGLResource*>			Resources;
	TMultiMap<DWORD,FOpenGLProgram*>		Programs;
	TMap<DWORD,GLuint>					Samplers;
	TMap<FName,BYTE>					HeatKinds;	// EOpenGLHeat by material name, the matching is too slow for every draw.

	GLuint	VertexArray,
			EmptyVertexArray,
			SceneBuffer,
			QuadIndices,
			VertexShaderObject,
			CopyProgram,
			OcclusionProgram,
			CompositeProgram,
			DepthOfFieldProgram,
			VolumetricProgram,
			ResolveProgram,
			OverlayProgram,
			IndirectProgram,
			ShadowAtlas,		// Distance maps of static lights, as fractions of their radius.
			SunShadow;			// Where the sun's shadow begins along its axis, world anchored and wrapping.
	GLsync	FrameFence;
	PFNGLDRAWELEMENTSBASEVERTEXOESPROC	DrawElementsBaseVertex;
	FOpenGLDynamicBuffer	DynamicVertexBuffer,
						DynamicIndexBuffer;
#ifdef SDL_PLATFORM_IOS
	// Apple's OpenGL ES waits for the GPU when a buffer it still reads is rewritten (gldFinishObject),
	// which froze iPhones for seconds a frame: each change of the scene block goes into a fresh slice.
	FOpenGLDynamicBuffer	SceneRing;
	GLint					UniformAlignment;
#endif

	// The world is drawn into World, scaled by ResolutionScale, and post processed into Scene. Scene then goes to the window.
	FOpenGLFramebuffer	Scene,
						World,
						Occlusion,				// Ambient occlusion and contact shadow at half the world size.
						Focus,					// The lit world with its signed circle of confusion in alpha.
						Scattering,				// Light scattered by the fog at half the world size.
						ScatterHistory[2],		// Scattering averaged over frames, alternating.
						Indirect,				// Light bounced off nearby surfaces at half the world size.
						IndirectHistory[2],		// Indirect averaged over frames, alternating.
						Overlay;				// Labels drawn within the world, laid over it after the post processing.
	AActor*				ShadowLight;			// Only compared, never dereferenced, the actor may be gone.
	DWORD				FrameCount;

	// The volumetric shadows are traced against the level's collision a little each frame. Static lights
	// keep their tile while they are among the recently used, the sun map follows the camera.
	struct FShadowTile
	{
		AActor*	Light;		// Only compared.
		FVector	Position;
		FLOAT	Radius;
		INT		Rows;		// Traced so far, the tile is used once all are.
		DOUBLE	Used;
	};
	FShadowTile			ShadowTiles[FOG_ATLAS_TILES * FOG_ATLAS_TILES];
	ULevel*				ShadowLevel;		// Only compared.
	FVector				SunDirection;
	INT					SunCells[FOG_SUN_SIZE * FOG_SUN_SIZE][2];
	UBOOL				SunPending[FOG_SUN_SIZE * FOG_SUN_SIZE];
	FLOAT				SunTexels[FOG_SUN_SIZE * FOG_SUN_SIZE];
	FMatrix				PrevWorldToCamera;
	FPlane				PrevProj;
	UBOOL				PrevViewValid,
						FrameViewValid,
						ScatterHistoryValid,
						IndirectHistoryValid;

	// Last state sent to GL, so Commit only changes what differs.
	struct FGLState
	{
		GLuint			Framebuffer,
						Program,
						Textures[OGL_MAX_TEXTURES],
						Samplers[OGL_MAX_TEXTURES],
						ElementBuffer;
		GLenum			TextureTargets[OGL_MAX_TEXTURES];
		INT				Viewport[4];
		UBOOL			Blend,
						DepthTest,
						Cull,
						PolygonOffset;
		GLenum			SrcBlend,
						DstBlend,
						DepthFunc,
						CullFace;
		GLboolean		DepthMask,
						AlphaMask;
		FLOAT			DepthNear,
						DepthFar;
		INT				ZBias;
		FOpenGLStream		Streams[OGL_MAX_STREAMS];
		INT				NumStreams,
						BaseVertex;
		UBOOL			Attributes[GLA_Max],
						BlackDiffuse;
		FOpenGLSceneBlock	Scene;
		UBOOL			Valid;
	} GL;

	UOpenGLRenderDevice();
	void StaticConstructor();

	// URenderDevice interface.
	virtual UBOOL Init();
	virtual UBOOL SetRes(UViewport* Viewport,INT NewX,INT NewY,UBOOL Fullscreen,INT ColorBytes=0,UBOOL bSaveSize=true);
	virtual void Exit(UViewport* Viewport);
	virtual void Flush(UViewport* Viewport);
	virtual void FlushResource(QWORD CacheId);
	virtual UBOOL ResourceCached(QWORD CacheId);
	virtual UBOOL Exec(const TCHAR* Cmd,FOutputDevice& Ar);
	virtual FRenderInterface* Lock(UViewport* Viewport,BYTE* HitData,INT* HitSize);
	virtual void Unlock(FRenderInterface* RI);
	virtual void Present(UViewport* Viewport);
	virtual void ReadPixels(UViewport* Viewport,FColor* Pixels,UBOOL Flipped);
	virtual void SetEmulationMode(EHardwareEmulationMode Mode) {}
	virtual FRenderCaps* GetRenderCaps();

	FOpenGLResource* GetCachedResource(QWORD CacheId);
	GLuint SmoothStream(FVertexStream* Source);
	void SetAlphaMask(GLboolean Alpha);
	void SetWorldDrawBuffers(UBOOL ReflectionOnly);
	GLuint GetSampler(DWORD Flags);
	GLuint CompileProgram(GLuint VertexShader,const ANSICHAR* FragmentSource);
	// Links without waiting, FinishProgram blocks until the result is known.
	GLuint StartProgram(GLuint VertexShader,const ANSICHAR* FragmentSource,GLuint& FragmentShader);
	GLuint FinishProgram(GLuint Program,GLuint FragmentShader);
	// Without Wait an unfinished program returns 0 and its draws are skipped.
	UBOOL ProgramReady(FOpenGLProgram* Program,UBOOL Wait);
	void ResolveFramebuffer(FOpenGLFramebuffer& F,UBOOL Depth = 0);
	void UseProgram(GLuint Program);
	// Covers the framebuffer with Program. Unit 2 is the depth texture, the others sample linearly.
	// Over lays premultiplied alpha over.
	void DrawFullscreen(GLuint Program,GLuint Framebuffer,INT Width,INT Height,GLuint Texture0,GLuint Texture1 = 0,GLuint Depth = 0,UBOOL Over = 0,GLuint Texture3 = 0,GLuint Texture4 = 0,GLuint Texture5 = 0,GLuint Texture6 = 0);
	void PostProcess(UBOOL ViewValid,const FMatrix& CameraToScreen,const FPlane& Light,FLOAT Gamma,FLOAT FocusDistance,const FOpenGLFogLights* Fog,const FPlane* Blasts);
	void FlushShadows();
	void CreateFramebuffer(FOpenGLFramebuffer& F,INT Width,INT Height,INT Samples,GLenum Format = GL_RGBA8,UBOOL HasDepth = 1,INT Extra = 0);
	GLuint Accumulate(FOpenGLFramebuffer& Input,FOpenGLFramebuffer History[2],UBOOL& Valid,GLenum Format,const FPlane& Proj,const FMatrix& Reproject);

private:
	UBOOL InitContext();
	void DetectOptions();
	UBOOL InitPostProcess(GLuint VertexShader);
	void ResetStateCache();
	void DestroyFramebuffer(FOpenGLFramebuffer& F);
	void ReleaseObjects();
};

extern const ANSICHAR* OpenGLVertexShader;
extern const ANSICHAR* OpenGLFragmentPrologue;
extern const ANSICHAR* FullscreenVertexShader;

#define AUTO_INITIALIZE_REGISTRANTS_OPENGLDRV UOpenGLRenderDevice::StaticClass();

#endif
