// Set calls only record into the state stack, Commit sends what differs from the last GL state
// right before each draw.

#include "OpenGLDrv.h"

FOpenGLRenderInterface::FOpenGLRenderInterface(UOpenGLRenderDevice* InRenDev)
:	RenDev(InRenDev), Viewport(NULL), SavedStateIndex(0), PrecacheMode(PRECACHE_All), FocusDiopters(0.f), FocusTime(0.0), ViewFog(0,0,0,0), FogUp(0,0,1), FogTime(0.0), DynamicStride(0), DynamicSize(0), DynamicComponents(0), DynamicIndices(NULL), DynamicIndicesFirst(0), ProbeMap(NAME_None)
{
	CurrentState = &SavedStates[0];
	FSavedState& S = *CurrentState;
	appMemzero(&S,sizeof(S));
	appMemzero(Blasts,sizeof(Blasts));
	S.LocalToWorld		= FMatrix::Identity;
	S.WorldToCamera		= FMatrix::Identity;
	S.CameraToScreen	= FMatrix::Identity;
	S.CullMode			= CM_None;
	S.UseStaticLighting	= 1;
	S.Material.Reset();
}

void FOpenGLRenderInterface::Locked(UViewport* InViewport)
{
	Viewport = InViewport;
	SavedStateIndex = 0;
	CurrentState = &SavedStates[0];
	CurrentState->RenderTarget		= NULL;
	CurrentState->ViewportX			= 0;
	CurrentState->ViewportY			= 0;
	CurrentState->ViewportWidth		= Viewport->SizeX;
	CurrentState->ViewportHeight	= Viewport->SizeY;
	InWorld = ViewModel = 0;
	PushState();
}

void FOpenGLRenderInterface::Unlocked()
{
	// A frame without HUD never draws in 2D.
	EndWorld();
	PopState();
	check(SavedStateIndex == 0);
}

void FOpenGLRenderInterface::PushState()
{
	check(SavedStateIndex + 1 < MAX_STATE_DEPTH);
	appMemcpy(&SavedStates[SavedStateIndex + 1],&SavedStates[SavedStateIndex],sizeof(FSavedState));
	CurrentState = &SavedStates[++SavedStateIndex];
}

void FOpenGLRenderInterface::PopState()
{
	if( SavedStateIndex == 0 )
		appErrorf(TEXT("PopState stack underflow"));
	CurrentState = &SavedStates[--SavedStateIndex];
}

FOpenGLTexture* FOpenGLRenderInterface::CacheTexture(FBaseTexture* Texture)
{
	FOpenGLTexture* Result = (FOpenGLTexture*)RenDev->GetCachedResource(Texture->GetCacheId());
	if( !Result )
		Result = new FOpenGLTexture(RenDev,Texture->GetCacheId());
	if( Result->CachedRevision != Texture->GetRevision() )
		Result->Cache(Texture);
	return Result;
}

UBOOL FOpenGLRenderInterface::SetRenderTarget(FRenderTarget* RenderTarget)
{
	FOpenGLTexture* Target = CacheTexture(RenderTarget);
	if( !Target->Framebuffer )
		return 0;
	CurrentState->RenderTarget		= Target;
	CurrentState->ViewportX			= 0;
	CurrentState->ViewportY			= 0;
	CurrentState->ViewportWidth		= Target->Width;
	CurrentState->ViewportHeight	= Target->Height;
	return 1;
}

void FOpenGLRenderInterface::SetViewport(INT X,INT Y,INT Width,INT Height)
{
	CurrentState->ViewportX			= X;
	CurrentState->ViewportY			= Y;
	CurrentState->ViewportWidth		= Width;
	CurrentState->ViewportHeight	= Height;
}

// Binds the target and sets viewport, scissor and depth range. D3D's top left origin flips the
// scene, and a scaled world maps the viewport's pixels onto fewer.
static void CommitTarget(FOpenGLRenderInterface* RI,FOpenGLRenderInterface::FSavedState* S,UBOOL ToOverlay = 0)
{
	UOpenGLRenderDevice*	RenDev = RI->RenDev;
	FOpenGLTexture*		Target = S->RenderTarget;
	INT					Rect[4] = { S->ViewportX, S->ViewportY, S->ViewportWidth, S->ViewportHeight };
	GLuint				Framebuffer;
	FLOAT				Near = 0.f, Far = 1.f;
	if( Target )
		Framebuffer = Target->Framebuffer;
	else
	{
		if( ToOverlay )
			RenDev->CreateFramebuffer(RenDev->Overlay,RenDev->Scene.Width,RenDev->Scene.Height,0,GL_RGBA8,0);
		FOpenGLFramebuffer&	Main = ToOverlay ? RenDev->Overlay : RI->InWorld ? RenDev->World : RenDev->Scene;
		INT					SW = RenDev->Scene.Width, SH = RenDev->Scene.Height,
							X0 = S->ViewportX * Main.Width / SW,
							Y0 = S->ViewportY * Main.Height / SH,
							X1 = ((S->ViewportX + S->ViewportWidth) * Main.Width + SW - 1) / SW,
							Y1 = ((S->ViewportY + S->ViewportHeight) * Main.Height + SH - 1) / SH;
		Framebuffer = Main.DrawFramebuffer();
		Rect[0] = X0;
		Rect[1] = Main.Height - Y1;
		Rect[2] = X1 - X0;
		Rect[3] = Y1 - Y0;
		// The weapon gets the front of the depth range, so walls never cut into it and the post
		// processing can tell its pixels apart.
		if( RI->InWorld && !ToOverlay )
		{
			Near = RI->ViewModel ? 0.f : VIEW_MODEL_DEPTH;
			Far = RI->ViewModel ? VIEW_MODEL_DEPTH : 1.f;
		}
	}
	UOpenGLRenderDevice::FGLState& GL = RenDev->GL;
	if( GL.Framebuffer != Framebuffer )
	{
		glBindFramebuffer(GL_FRAMEBUFFER,Framebuffer);
		GL.Framebuffer = Framebuffer;
	}
	if( appMemcmp(Rect,GL.Viewport,sizeof(Rect)) != 0 )
	{
		glViewport(Rect[0],Rect[1],Rect[2],Rect[3]);
		glScissor(Rect[0],Rect[1],Rect[2],Rect[3]);
		appMemcpy(GL.Viewport,Rect,sizeof(Rect));
	}
	if( GL.DepthNear != Near || GL.DepthFar != Far )
	{
		glDepthRangef(Near,Far);
		GL.DepthNear = Near;
		GL.DepthFar = Far;
	}
	if( ToOverlay && !RI->OverlayUsed )
	{
		glDisable(GL_SCISSOR_TEST);
		// Clears obey the colour mask, and the world draw before this one may have left alpha off.
		RenDev->SetAlphaMask(GL_TRUE);
		glClearColor(0.f,0.f,0.f,0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_SCISSOR_TEST);
		RI->OverlayUsed = 1;
	}
}

void FOpenGLRenderInterface::BeginWorld()
{
	InWorld = 1;
	ViewModel = WorldDrawn = OverlayUsed = 0;
	ViewFog = FPlane(0,0,0,0);
	WorldLights.Empty();
	// The engine's clears before the world began reached Scene.
	Clear(1,FColor(0,0,0),1,1.f,0,0);
}

void FOpenGLRenderInterface::BeginViewModel(const FMatrix& WorldToCamera,const FMatrix& CameraToScreen)
{
	if( !InWorld )
		return;
	DrawDeferred();
	ViewModel			= 1;
	ViewWorldToCamera	= WorldToCamera;
	ViewCameraToScreen	= CameraToScreen;
}

// Post processing only applies to the world. The HUD and menus draw over it unchanged and with
// their own depth.
void FOpenGLRenderInterface::EndWorld()
{
	if( !InWorld )
		return;
	DrawDeferred();
	// Depth needs the player's projection and the level in the world framebuffer, letterboxing
	// moves the projection off the framebuffer.
	UBOOL ViewValid = ViewModel && WorldDrawn && !Viewport->bRenderCinematics;
	InWorld = ViewModel = 0;
	RenDev->ResolveFramebuffer(RenDev->World,1);
	FPlane Light(0,0,0,0);
	if( ViewValid && RenDev->ContactShadows )
		Light = PickShadowLight();
	FPlane BlastWaves[2 * HEAT_BLASTS];
	appMemzero(BlastWaves,sizeof(BlastWaves));
	if( ViewValid && RenDev->HeatHaze )
		PickBlasts(BlastWaves);
	FLOAT Gamma = ParseParam(appCmdLine(),TEXT("NOGAMMA")) ? 1.f : Viewport->GetOuterUClient()->Gamma;
	RenDev->PostProcess(ViewValid,ViewCameraToScreen,Light,Gamma,ViewValid && RenDev->DepthOfField ? PickFocus() : 0.f,
		ViewValid && RenDev->VolumetricFog ? PickFogLights() : NULL,BlastWaves);
	if( OverlayUsed )
		RenDev->DrawFullscreen(RenDev->OverlayProgram,RenDev->Scene.DrawFramebuffer(),RenDev->Scene.Width,RenDev->Scene.Height,RenDev->Overlay.Texture,0,0,1);
	OverlayUsed = 0;
	WorldLights.Empty();
	Clear(0,FColor(0,0,0),1,1.f,0,0);
}

void FOpenGLRenderInterface::Clear(UBOOL UseColor,FColor Color,UBOOL UseDepth,FLOAT Depth,UBOOL UseStencil,DWORD Stencil)
{
	if( ViewModel && !CurrentState->RenderTarget )
	{
		// The HUD clears depth for the weapon, its depth range already keeps it in front.
		if( !UseColor )
			return;
		EndWorld();
	}
	// Sky zones and mirrors draw first and the level clears depth over them, their beams belong to their depth.
	if( UseDepth && InWorld && !CurrentState->RenderTarget )
		DrawDeferred();
	CommitTarget(this,CurrentState);
	GLbitfield Mask = 0;
	if( UseColor )
	{
		// The world's alpha is the heat haze mask, nothing is hot yet.
		RenDev->SetAlphaMask(GL_TRUE);
		glClearColor(Color.R / 255.f,Color.G / 255.f,Color.B / 255.f,InWorld && !CurrentState->RenderTarget ? 0.f : Color.A / 255.f);
		Mask |= GL_COLOR_BUFFER_BIT;
	}
	if( UseDepth )
	{
		// Clears obey the write masks, D3D's do not.
		glDepthMask(GL_TRUE);
		RenDev->GL.DepthMask = GL_TRUE;
		glClearDepthf(Depth);
		Mask |= GL_DEPTH_BUFFER_BIT;
	}
	if( Mask )
		glClear(Mask);
	// Cleared pixels reflect and bounce nothing, whatever the colour. Disabled draw buffers ignore the clear.
	if( UseColor && InWorld && !CurrentState->RenderTarget )
	{
		GLfloat Zero[4] = { 0.f, 0.f, 0.f, 0.f };
		glClearBufferfv(GL_COLOR,1,Zero);
		glClearBufferfv(GL_COLOR,2,Zero);
	}
}

void FOpenGLRenderInterface::SetCullMode(ECullMode CullMode)
{
	CurrentState->CullMode = CullMode;
}

void FOpenGLRenderInterface::SetAmbientLight(FColor Color)
{
	CurrentState->AmbientLightColor = Color;
}

void FOpenGLRenderInterface::EnableLighting(UBOOL UseDynamic,UBOOL UseStatic,UBOOL Modulate2X,FBaseTexture* Lightmap,UBOOL LightingOnly,FSphere LitSphere)
{
	CurrentState->UseDynamicLighting	= UseDynamic;
	CurrentState->UseStaticLighting		= UseStatic;
	CurrentState->LightingModulate2X	= Modulate2X;
	CurrentState->Lightmap				= Lightmap ? CacheTexture(Lightmap) : NULL;
	CurrentState->LightingOnly			= LightingOnly;
	CurrentState->LitSphere				= LitSphere;
}

void FOpenGLRenderInterface::SetLight(INT LightIndex,FDynamicLight* Light,FLOAT Scale)
{
	if( LightIndex >= OGL_MAX_LIGHTS )
		return;
	CurrentState->LightEnabled[LightIndex] = Light != NULL;
	if( !Light )
		return;
	// Terrain passes lights from its stack, only those an actor owns outlive the call.
	if( InWorld && Light->Actor && Light->Actor->LightRenderData == Light )
		WorldLights.AddUniqueItem(Light);

	BYTE	Effect = Light->Actor ? Light->Actor->LightEffect : LE_None;
	FLOAT	Type;
	if( Effect == LE_Sunlight )
		Type = 0.f;
	else if( Effect == LE_QuadraticNonIncidence || CurrentState->LitSphere.W == -1.f )
		Type = 3.f;
	else if( Effect == LE_Cylinder )
		Type = 1.f;
	else if( Effect == LE_NonIncidence )
		Type = 2.f;
	else if( Effect == LE_Spotlight || Effect == LE_StaticSpot )
		Type = 5.f;
	else
		Type = 4.f;

	FLOAT		Brightness = Light->Alpha * Scale * (Effect == LE_Negative ? -1.f : 1.f);
	FOpenGLLight&	Out = CurrentState->Lights[LightIndex];
	Out.Position	= FPlane(Light->Position,Light->Radius);
	Out.Direction	= FPlane(Light->Direction,0.f);
	Out.Color		= FPlane(Light->Color.X * Brightness,Light->Color.Y * Brightness,Light->Color.Z * Brightness,0.f);
	Out.Params		= FPlane(Type,Light->Actor ? 1.f - Light->Actor->LightCone / 256.f : 0.f,0.f,0.f);
}

void FOpenGLRenderInterface::SetDistanceFog(UBOOL Enable,FLOAT FogStart,FLOAT FogEnd,FColor Color)
{
	CurrentState->FogEnabled	= Enable;
	CurrentState->FogStart		= Min(FogStart,FogEnd - 1.f);
	CurrentState->FogEnd		= FogEnd;
	CurrentState->FogColor		= Color;
	// The level sets the fog of the player's zone first, its sky, mirrors and coronas follow.
	if( InWorld && !ViewModel && !CurrentState->RenderTarget && ViewFog.W == 0.f )
	{
		ViewFog			= FPlane(CurrentState->FogStart,FogEnd,Enable ? 1.f : 0.f,1.f);
		ViewFogColor	= Color.Plane();
	}
}

void FOpenGLRenderInterface::SetGlobalColor(FColor Color)
{
	FOpenGLMaterialState& Material = CurrentState->Material;
	if( Material.GlobalColorConstant != INDEX_NONE )
		Material.Constants[Material.GlobalColorConstant] = Color.Plane();
}

void FOpenGLRenderInterface::SetTransform(ETransformType Type,const FMatrix& Matrix)
{
	if( Type == TT_LocalToWorld )
		CurrentState->LocalToWorld = Matrix;
	else if( Type == TT_WorldToCamera )
		CurrentState->WorldToCamera = Matrix;
	else if( Type == TT_CameraToScreen )
		CurrentState->CameraToScreen = Matrix;
}

void FOpenGLRenderInterface::SetZBias(INT ZBias)
{
	CurrentState->ZBias = ZBias;
}

INT FOpenGLRenderInterface::SetVertexStreams(EVertexShader Shader,FVertexStream** Streams,INT NumStreams)
{
	INT Size = 0;
	NumStreams = Min<INT>(NumStreams,OGL_MAX_STREAMS);
	for( INT i=0; i<NumStreams; i++ )
	{
		FOpenGLVertexStream* Stream = (FOpenGLVertexStream*)RenDev->GetCachedResource(Streams[i]->GetCacheId());
		if( !Stream )
			Stream = new FOpenGLVertexStream(RenDev,Streams[i]->GetCacheId());
		if( Stream->CachedRevision != Streams[i]->GetRevision() )
		{
			Size += Streams[i]->GetSize();
			Stream->Cache(Streams[i]);
		}
		FOpenGLStream& Out = CurrentState->Streams[i];
		Out.Buffer			= Stream->Buffer;
		Out.Stride			= Streams[i]->GetStride();
		Out.NumComponents	= Streams[i]->GetComponents(Out.Components);
	}
	CurrentState->NumStreams = NumStreams;
	CurrentState->Source = NumStreams ? Streams[0] : NULL;
	return Size;
}

INT FOpenGLRenderInterface::UseDynamicVertices(INT Offset,INT Stride,const FVertexComponent* Components,INT NumComponents)
{
	FOpenGLStream& Out = CurrentState->Streams[0];
	Out.Buffer			= RenDev->DynamicVertexBuffer.Buffer;
	Out.Stride			= Stride;
	Out.NumComponents	= NumComponents;
	appMemcpy(Out.Components,Components,NumComponents * sizeof(FVertexComponent));
	CurrentState->NumStreams = 1;
	CurrentState->Source = NULL;
	return Offset / Stride;
}

INT FOpenGLRenderInterface::SetDynamicStream(EVertexShader Shader,FVertexStream* Stream)
{
	FVertexComponent Components[MAX_VERTEX_COMPONENTS];
	INT NumComponents = Stream->GetComponents(Components), Stride = Stream->GetStride(), Offset;
	if( BYTE* Dest = RenDev->DynamicVertexBuffer.Lock(Stream->GetSize(),Stride,Offset) )
		Stream->GetStreamData(Dest);
	RenDev->DynamicVertexBuffer.Unlock();
	return UseDynamicVertices(Offset,Stride,Components,NumComponents);
}

INT FOpenGLRenderInterface::SetIndexBuffer(FIndexBuffer* IndexBuffer,INT BaseVertexIndex)
{
	if( !IndexBuffer )
	{
		CurrentState->IndexBuffer		= 0;
		CurrentState->IndexBufferBase	= 0;
		return 0;
	}

	INT Size = 0;
	FOpenGLIndexBuffer* Buffer = (FOpenGLIndexBuffer*)RenDev->GetCachedResource(IndexBuffer->GetCacheId());
	if( !Buffer )
		Buffer = new FOpenGLIndexBuffer(RenDev,IndexBuffer->GetCacheId());
	if( Buffer->CachedRevision != IndexBuffer->GetRevision() )
	{
		Size = IndexBuffer->GetSize();
		Buffer->Cache(IndexBuffer);
	}
	CurrentState->IndexBuffer		= Buffer->Buffer;
	CurrentState->IndexSize			= IndexBuffer->GetIndexSize();
	CurrentState->IndexBufferBase	= BaseVertexIndex;
	return Size;
}

INT FOpenGLRenderInterface::SetDynamicIndexBuffer(FIndexBuffer* IndexBuffer,INT BaseVertexIndex)
{
	INT IndexSize = IndexBuffer->GetIndexSize(), Offset;
	if( BYTE* Dest = RenDev->DynamicIndexBuffer.Lock(IndexBuffer->GetSize(),IndexSize,Offset) )
		IndexBuffer->GetContents(Dest);
	RenDev->DynamicIndexBuffer.Unlock();
	DynamicIndices					= IndexBuffer;
	DynamicIndicesFirst				= Offset / IndexSize;
	RenDev->GL.ElementBuffer		= RenDev->DynamicIndexBuffer.Buffer;
	CurrentState->IndexBuffer		= RenDev->DynamicIndexBuffer.Buffer;
	CurrentState->IndexSize			= IndexSize;
	CurrentState->IndexBufferBase	= BaseVertexIndex;
	return Offset / IndexSize;
}

int FOpenGLRenderInterface::LockDynBuffer(BYTE** OutBuffer,int NumVerts,int Stride,DWORD ComponentFlags)
{
	if( NumVerts <= 0 )
	{
		*OutBuffer = NULL;
		return 0;
	}
	NumVerts = Min(NumVerts,OGL_MAX_QUADS * 4 - 1);
	if( DynamicVertices.Num() < NumVerts * Stride )
		DynamicVertices.Add(NumVerts * Stride - DynamicVertices.Num());
	DynamicStride		= Stride;
	DynamicSize			= NumVerts * Stride;
	DynamicComponents	= ComponentFlags;
	*OutBuffer = &DynamicVertices(0);
	return NumVerts;
}

int FOpenGLRenderInterface::UnlockDynBuffer()
{
	FVertexComponent	Components[MAX_VERTEX_COMPONENTS];
	INT					NumComponents = 0;
	if( DynamicComponents & VF_Position )	Components[NumComponents++] = FVertexComponent(CT_Float3,FVF_Position);
	if( DynamicComponents & VF_Normal )		Components[NumComponents++] = FVertexComponent(CT_Float3,FVF_Normal);
	if( DynamicComponents & VF_Diffuse )	Components[NumComponents++] = FVertexComponent(CT_Color,FVF_Diffuse);
	if( DynamicComponents & VF_Specular )	Components[NumComponents++] = FVertexComponent(CT_Color,FVF_Specular);
	if( DynamicComponents & VF_Tex1 )		Components[NumComponents++] = FVertexComponent(CT_Float2,FVF_TexCoord0);
	if( DynamicComponents & VF_Tex2 )		Components[NumComponents++] = FVertexComponent(CT_Float2,FVF_TexCoord1);
	INT Offset;
	if( BYTE* Dest = RenDev->DynamicVertexBuffer.Lock(DynamicSize,DynamicStride,Offset) )
		appMemcpy(Dest,&DynamicVertices(0),DynamicSize);
	RenDev->DynamicVertexBuffer.Unlock();
	return UseDynamicVertices(Offset,DynamicStride,Components,NumComponents);
}

void FOpenGLRenderInterface::SetQuadIndexBuffer(INT BaseVertexIndex)
{
	if( !RenDev->QuadIndices )
	{
		TArray<_WORD> Indices(OGL_MAX_QUADS * 6);
		for( INT i=0; i<OGL_MAX_QUADS; i++ )
		{
			_WORD* Quad = &Indices(i * 6);
			Quad[0] = i * 4; Quad[1] = i * 4 + 1; Quad[2] = i * 4 + 2;
			Quad[3] = i * 4; Quad[4] = i * 4 + 2; Quad[5] = i * 4 + 3;
		}
		glGenBuffers(1,&RenDev->QuadIndices);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,RenDev->QuadIndices);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,Indices.Num() * sizeof(_WORD),&Indices(0),GL_STATIC_DRAW);
		RenDev->GL.ElementBuffer = RenDev->QuadIndices;
	}
	CurrentState->IndexBuffer		= RenDev->QuadIndices;
	CurrentState->IndexSize			= sizeof(_WORD);
	CurrentState->IndexBufferBase	= BaseVertexIndex;
}

void FOpenGLRenderInterface::DrawDynQuads(INT NumPrimitives)
{
	DrawQuads(UnlockDynBuffer(),NumPrimitives);
}

void FOpenGLRenderInterface::DrawQuads(INT FirstVertex,INT NumPrimitives)
{
	if( !NumPrimitives )
		return;
	SetQuadIndexBuffer(FirstVertex);
	DrawPrimitive(PT_TriangleList,0,NumPrimitives * 2,0,NumPrimitives * 4 - 1);
}

static void SetCap(GLenum Cap,UBOOL Enable,UBOOL& Cached)
{
	if( Cached != Enable )
	{
		if( Enable )
			glEnable(Cap);
		else
			glDisable(Cap);
		Cached = Enable;
	}
}

// The content keeps no capture point for its cubemaps, so the box is fitted by traces into the level
// from the drawn object's origin. The median of five rays per side keeps one ray through a gap from
// opening the side. Like the fog shadows they pass the skybox, so the sky counts as infinitely far
// and the sky zone stays a scene of its own.
FOpenGLRenderInterface::FProbe FOpenGLRenderInterface::FindProbe(const FMatrix& LocalToWorld)
{
	const FLOAT	Far = 1e8f;
	FProbe		Probe = { FVector(0,0,0), FVector(-Far,-Far,-Far), FVector(Far,Far,Far) };
	ULevel*		Level = Viewport->Actor ? Viewport->Actor->XLevel : NULL;
	// BSP, terrain and world space particles have no object to centre on.
	if( !Level || ViewModel || LocalToWorld == FMatrix::Identity )
		return Probe;
	// By map name, a new level can reuse the old one's address.
	if( Level->GetOuter()->GetFName() != ProbeMap || Probes.Num() >= 256 )
	{
		Probes.Empty();
		ProbeMap = Level->GetOuter()->GetFName();
	}
	FVector	C = LocalToWorld.TransformFVector(FVector(0,0,0));
	QWORD	Key = 0;
	for( INT Axis=0; Axis<3; Axis++ )
		Key = (Key << 21) | (appRound(C.Component(Axis) / 16.f) & 0x1fffff);
	if( FProbe* Found = Probes.Find(Key) )
		return *Found;

	Probe.Origin = C;
	for( INT Axis=0; Axis<3; Axis++ )
		for( INT Sign=-1; Sign<=1; Sign+=2 )
		{
			FVector	A(0,0,0), U(0,0,0), V(0,0,0);
			A.Component(Axis) = Sign;
			U.Component(Axis == 0 ? 1 : 0) = 0.5f;
			V.Component(Axis == 2 ? 1 : 2) = 0.5f;
			// Water and floors reflect what stands above them, so the sides are traced level and upward.
			// Downward rays find the bed or the bank right at the waterline.
			FVector	Dirs[5] = { A, A + U, A - U, A + V, Axis == 2 ? A - V : A + V * 2.f };
			FLOAT	Distances[5];
			for( INT i=0; i<5; i++ )
			{
				FVector			Dir = Dirs[i].SafeNormal();
				FCheckResult	Hit(1.f);
				Level->SingleLineCheck(Hit,NULL,C + Dir * WORLD_MAX,C,TRACE_ShadowCast | TRACE_Level | TRACE_LevelGeometry);
				FLOAT D = Hit.Time * WORLD_MAX * Abs(Dir.Component(Axis));
				INT j = i;
				for( ; j>0 && Distances[j - 1] > D; j-- )
					Distances[j] = Distances[j - 1];
				Distances[j] = D;
			}
			(Sign < 0 ? Probe.Min : Probe.Max).Component(Axis) = C.Component(Axis) + Sign * Max(Distances[2],64.f);
		}
	// Cubemaps were captured from the middle of the space around them rather than from an object's
	// pivot, which for a fluid surface is the centre of a grid that mostly runs under the terrain.
	for( INT Axis=0; Axis<3; Axis++ )
		if( Probe.Max.Component(Axis) - Probe.Min.Component(Axis) < WORLD_MAX * 0.5f )
			Probe.Origin.Component(Axis) = (Probe.Min.Component(Axis) + Probe.Max.Component(Axis)) * 0.5f;
	Probes.Set(Key,Probe);
	return Probe;
}

void FOpenGLRenderInterface::Commit()
{
	UOpenGLRenderDevice::FGLState&	GL = RenDev->GL;
	FSavedState*					S = CurrentState;
	FOpenGLMaterialState&				M = S->Material;
	FOpenGLProgram*					Program = M.Program;

	// The first 2D draw after the weapon ends the world.
	UBOOL Perspective = S->CameraToScreen.M[2][3] == 1.f;
	if( InWorld && !S->RenderTarget )
	{
		if( !ViewModel )
			WorldDrawn |= Perspective;
		else if( !Perspective )
			EndWorld();
	}
	// Labels the level draws over itself stay out of its post processing, like the HUD. The overlay
	// keeps premultiplied alpha, which holds blending over and adding but not multiplying.
	UBOOL ToOverlay = InWorld && !ViewModel && WorldDrawn && !Perspective && !S->RenderTarget
		&& (M.DstBlend == GL_ONE_MINUS_SRC_ALPHA || M.DstBlend == GL_ONE);
	CommitTarget(this,S,ToOverlay);

	RenDev->UseProgram(Program->Program);

	FOpenGLSceneBlock	Scene;
	FLOAT			Flip = S->RenderTarget ? -1.f : 1.f;
	appMemzero(&Scene,sizeof(Scene));
	Scene.WorldToCamera		= S->WorldToCamera;
	Scene.CameraToScreen	= S->CameraToScreen;
	Scene.ClipAdjust		= FPlane(Flip,0.f,1.f / Max(GL.Viewport[2],1),-Flip / Max(GL.Viewport[3],1));
	Scene.Fog				= FPlane(S->FogStart,S->FogEnd,S->FogEnabled ? 1.f : 0.f,RenDev->VolumetricFog ? 1.f : 0.f);
	Scene.FogColor			= S->FogColor.Plane();
	Scene.Options			= FPlane(S->UseDynamicLighting ? 1.f : 0.f,S->UseStaticLighting ? 1.f : 0.f,S->LightingModulate2X ? 1.f : 0.f,0.f);
	// Particles pass camera space vertices with no view transform, they keep the up of the view before them.
	if( S->WorldToCamera != FMatrix::Identity )
		FogUp = S->WorldToCamera.TransformNormal(FVector(0,0,1)).SafeNormal();
	// W tells solid draws they are in the world, where they put out the heat of what they cover.
	UBOOL WorldTarget = InWorld && !S->RenderTarget && !ToOverlay;
	Scene.FogUp				= FPlane(FogUp,WorldTarget ? 1.f : 0.f);
	INT NumLights = 0;
	for( INT i=0; i<OGL_MAX_LIGHTS; i++ )
	{
		if( !S->LightEnabled[i] )
			continue;
		Scene.LightPosition[NumLights]	= S->Lights[i].Position;
		Scene.LightDirection[NumLights]	= S->Lights[i].Direction;
		Scene.LightColor[NumLights]		= S->Lights[i].Color;
		Scene.LightParams[NumLights]	= S->Lights[i].Params;
		NumLights++;
	}
	FPlane Ambient = S->AmbientLightColor.Plane();
	Scene.Ambient = FPlane(Ambient.X,Ambient.Y,Ambient.Z,NumLights);
	if( !GL.Valid || appMemcmp(&Scene,&GL.Scene,sizeof(Scene)) != 0 )
	{
#ifdef SDL_PLATFORM_IOS
		// A fresh slice each time: rewriting a block the GPU still reads makes Apple's driver wait for it.
		INT Offset = 0;
		if( BYTE* Dest = RenDev->SceneRing.Lock(sizeof(Scene),RenDev->UniformAlignment,Offset) )
		{
			appMemcpy(Dest,&Scene,sizeof(Scene));
			RenDev->SceneRing.Unlock();
			glBindBufferRange(GL_UNIFORM_BUFFER,0,RenDev->SceneRing.Buffer,Offset,sizeof(Scene));
		}
#else
		glBindBuffer(GL_UNIFORM_BUFFER,RenDev->SceneBuffer);
		glBufferSubData(GL_UNIFORM_BUFFER,0,sizeof(Scene),&Scene);
#endif
		GL.Scene = Scene;
		GL.Valid = 1;
	}

	// Per draw uniforms. Normals take the inverse transpose, so scaled meshes light correctly.
	if( appMemcmp(&Program->SentLocalToWorld,&S->LocalToWorld,sizeof(FMatrix)) != 0 )
	{
		Program->SentLocalToWorld = S->LocalToWorld;
		FMatrix Inverse = S->LocalToWorld.Inverse();
		FLOAT	NormalToWorld[9];
		for( INT Row=0; Row<3; Row++ )
			for( INT Column=0; Column<3; Column++ )
				NormalToWorld[Row * 3 + Column] = Inverse.M[Column][Row];
		glUniformMatrix4fv(Program->LocalToWorld,1,GL_FALSE,&S->LocalToWorld.M[0][0]);
		glUniformMatrix3fv(Program->NormalToWorld,1,GL_FALSE,NormalToWorld);
		if( Program->Probe != -1 )
		{
			FProbe Probe = FindProbe(S->LocalToWorld);
			glUniform3fv(Program->Probe,3,&Probe.Origin.X);
		}
	}
	if( M.AlphaTest && Program->SentAlphaRef != M.AlphaRef )
	{
		Program->SentAlphaRef = M.AlphaRef;
		glUniform1f(Program->AlphaRef,M.AlphaRef);
	}
	for( INT i=0; i<M.NumMatrices; i++ )
		if( appMemcmp(&Program->SentMatrices[i],&M.Matrices[i],sizeof(FMatrix)) != 0 )
		{
			Program->SentMatrices[i] = M.Matrices[i];
			glUniformMatrix4fv(Program->Matrices[i],1,GL_FALSE,&M.Matrices[i].M[0][0]);
		}
	for( INT i=0; i<M.NumConstants; i++ )
		if( appMemcmp(&Program->SentConstants[i],&M.Constants[i],sizeof(FPlane)) != 0 )
		{
			Program->SentConstants[i] = M.Constants[i];
			glUniform4fv(Program->Constants[i],1,&M.Constants[i].X);
		}

	for( INT i=0; i<M.NumTextures; i++ )
	{
		FOpenGLTexture*	Texture = M.Textures[i];
		GLuint			Name = Texture ? Texture->Texture : 0;
		GLenum			Target = Texture ? Texture->Target : GL_TEXTURE_2D;
		if( GL.Textures[i] != Name || GL.TextureTargets[i] != Target )
		{
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(Target,Name);
			GL.Textures[i]			= Name;
			GL.TextureTargets[i]	= Target;
		}
		GLuint Sampler = RenDev->GetSampler(M.SamplerFlags[i] | (Texture && Texture->NumMips > 1 ? GSF_Mipmapped : 0));
		if( GL.Samplers[i] != Sampler )
		{
			glBindSampler(i,Sampler);
			GL.Samplers[i] = Sampler;
		}
	}

	UBOOL Blend = !(M.SrcBlend == GL_ONE && M.DstBlend == GL_ZERO);
	// The world's alpha is the heat haze mask. Hot draws add to it and solid ones clear it, as the
	// level's draw order may put the lava under a floor before the floor. Blended ones in front, like
	// smoke, leave it. The mask covers every attachment, and of the blended draws only those that scale
	// what is beneath them change the reflection's alpha, so they keep writing.
	UBOOL Scales = M.DstBlend == GL_SRC_COLOR || (M.SrcBlend == GL_ZERO && M.DstBlend == GL_ONE_MINUS_SRC_COLOR);
	RenDev->SetAlphaMask(!WorldTarget || !RenDev->HeatHaze || M.Heat || !Blend || Scales);
	SetCap(GL_BLEND,Blend,GL.Blend);
	if( Blend && ToOverlay )
	{
		// Adding leaves what is beneath visible.
		UBOOL Covers = M.DstBlend != GL_ONE;
		glBlendFuncSeparate(M.SrcBlend,M.DstBlend,Covers ? GL_ONE : GL_ZERO,Covers ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
		GL.SrcBlend = GL.DstBlend = (GLenum)-1;
	}
	else if( Blend && (GL.SrcBlend != M.SrcBlend || GL.DstBlend != M.DstBlend) )
	{
		glBlendFunc(M.SrcBlend,M.DstBlend);
		GL.SrcBlend = M.SrcBlend;
		GL.DstBlend = M.DstBlend;
	}

	GLenum DepthFunc = M.ZTest ? GL_LEQUAL : GL_ALWAYS;
	if( GL.DepthFunc != DepthFunc )
	{
		glDepthFunc(DepthFunc);
		GL.DepthFunc = DepthFunc;
	}
	GLboolean DepthMask = M.ZWrite ? GL_TRUE : GL_FALSE;
	if( GL.DepthMask != DepthMask )
	{
		glDepthMask(DepthMask);
		GL.DepthMask = DepthMask;
	}

	// D3D culls by on screen winding, the render target flip reverses it.
	UBOOL Cull = !M.TwoSided && S->CullMode != CM_None;
	SetCap(GL_CULL_FACE,Cull,GL.Cull);
	GLenum CullFace = ((S->CullMode == CM_CW) != (S->RenderTarget != NULL)) ? GL_BACK : GL_FRONT;
	if( Cull && GL.CullFace != CullFace )
	{
		glCullFace(CullFace);
		GL.CullFace = CullFace;
	}

	SetCap(GL_POLYGON_OFFSET_FILL,S->ZBias != 0,GL.PolygonOffset);
	if( S->ZBias && GL.ZBias != S->ZBias )
	{
		// D3D9Drv's depth bias, the units are one 24 bit depth step.
		FLOAT Bias = -5.f * S->ZBias / FAR_CLIPPING_PLANE;
		glPolygonOffset(Bias,Bias * 16777216.f);
		GL.ZBias = S->ZBias;
	}
}

static UBOOL SameStream(const FOpenGLStream& A,const FOpenGLStream& B)
{
	// Components past NumComponents are stale.
	return A.Buffer == B.Buffer && A.Stride == B.Stride && A.NumComponents == B.NumComponents
		&& appMemcmp(A.Components,B.Components,A.NumComponents * sizeof(FVertexComponent)) == 0;
}

// Points the attributes at the streams. Without base vertex draws the base goes into the offsets.
static void CommitStreams(UOpenGLRenderDevice* RenDev,FOpenGLRenderInterface::FSavedState* S,INT BaseVertex)
{
	UOpenGLRenderDevice::FGLState& GL = RenDev->GL;
	if( GL.NumStreams == S->NumStreams && GL.BaseVertex == BaseVertex )
	{
		INT i = 0;
		while( i < S->NumStreams && SameStream(GL.Streams[i],S->Streams[i]) )
			i++;
		if( i == S->NumStreams )
			return;
	}

	UBOOL Used[GLA_Max];
	appMemzero(Used,sizeof(Used));
	for( INT StreamIndex=0; StreamIndex<S->NumStreams; StreamIndex++ )
	{
		FOpenGLStream& Stream = S->Streams[StreamIndex];
		glBindBuffer(GL_ARRAY_BUFFER,Stream.Buffer);
		INT Offset = BaseVertex * Stream.Stride;
		for( INT i=0; i<Stream.NumComponents; i++ )
		{
			FVertexComponent&	Component = Stream.Components[i];
			INT					Size = Component.Type == CT_Color ? 4 : 4 - Component.Type;
			INT					Location = Component.Function == FVF_Position ? GLA_Position
										: Component.Function == FVF_Normal ? GLA_Normal
										: Component.Function == FVF_Diffuse ? GLA_Diffuse
										: Component.Function == FVF_Specular ? GLA_Specular
										: GLA_TexCoord0 + (Component.Function - FVF_TexCoord0);
			if( Component.Type == CT_Color )
				glVertexAttribPointer(Location,4,GL_UNSIGNED_BYTE,GL_TRUE,Stream.Stride,(void*)(PTRINT)Offset);
			else
				glVertexAttribPointer(Location,Size,GL_FLOAT,GL_FALSE,Stream.Stride,(void*)(PTRINT)Offset);
			Used[Location] = 1;
			Offset += Component.Type == CT_Color ? 4 : Size * sizeof(FLOAT);
		}
	}
	for( INT i=0; i<GLA_Max; i++ )
	{
		if( GL.Attributes[i] != Used[i] )
		{
			if( Used[i] )
				glEnableVertexAttribArray(i);
			else
				glDisableVertexAttribArray(i);
			GL.Attributes[i] = Used[i];
		}
	}
	appMemcpy(GL.Streams,S->Streams,S->NumStreams * sizeof(FOpenGLStream));
	GL.NumStreams	= S->NumStreams;
	GL.BaseVertex	= BaseVertex;
}

void FOpenGLRenderInterface::DrawPrimitive(EPrimitiveType PrimitiveType,INT FirstIndex,INT NumPrimitives,INT MinIndex,INT MaxIndex)
{
	guard(FOpenGLRenderInterface::DrawPrimitive);

	FOpenGLProgram* Program = CurrentState->Material.Program;
	// Render targets may be drawn only once, so they wait instead of keeping the hole.
	if( NumPrimitives <= 0 || !Program || !RenDev->ProgramReady(Program,CurrentState->RenderTarget != NULL) || !Program->Program )
		return;

	// The engine sorts translucent actors by their centre, which puts long beams behind foliage they
	// stand in front of. Adding is order independent, so beams wait until the level is done.
	FSavedState* S = CurrentState;
	if( S->Material.Beam && InWorld && !ViewModel && !S->RenderTarget && S->Streams[0].Buffer != RenDev->DynamicVertexBuffer.Buffer )
	{
		FDeferredDraw& D	= DeferredDraws(DeferredDraws.Add());
		D.State				= *S;
		// Flat faces would each get one brightness, the soft edge needs normals that turn across them.
		if( S->Source )
			D.State.Streams[0].Buffer = RenDev->SmoothStream(S->Source);
		D.Type				= PrimitiveType;
		D.FirstIndex		= FirstIndex;
		D.NumPrimitives		= NumPrimitives;
		D.MinIndex			= MinIndex;
		D.MaxIndex			= MaxIndex;
		D.IndexBytes		= 0;
		if( S->IndexBuffer && S->IndexBuffer == RenDev->DynamicIndexBuffer.Buffer )
		{
			D.IndexBytes	= DynamicIndices->GetSize();
			D.IndexOffset	= DeferredIndices.Add(D.IndexBytes);
			D.FirstIndex	-= DynamicIndicesFirst;
			DynamicIndices->GetContents(&DeferredIndices(D.IndexOffset));
		}
		return;
	}

	GLenum	Mode;
	INT		Count;
	switch( PrimitiveType )
	{
	case PT_TriangleStrip:	Mode = GL_TRIANGLE_STRIP;	Count = NumPrimitives + 2; break;
	case PT_TriangleFan:	Mode = GL_TRIANGLE_FAN;		Count = NumPrimitives + 2; break;
	case PT_PointList:		Mode = GL_POINTS;			Count = NumPrimitives; break;
	case PT_LineList:		Mode = GL_LINES;			Count = NumPrimitives * 2; break;
	default:				Mode = GL_TRIANGLES;		Count = NumPrimitives * 3; break;
	}

	Commit();
	UBOOL Indexed = CurrentState->IndexBuffer != 0;
	INT Base = Indexed ? CurrentState->IndexBufferBase : 0, DrawBase = RenDev->DrawElementsBaseVertex ? Base : 0;
	CommitStreams(RenDev,CurrentState,Base - DrawBase);

	// Without a colour stream D3D lights with its black material emissive, and unlit draws get white.
	UBOOL BlackDiffuse = CurrentState->UseDynamicLighting;
	if( !RenDev->GL.Attributes[GLA_Diffuse] && RenDev->GL.BlackDiffuse != BlackDiffuse )
	{
		FLOAT Value = BlackDiffuse ? 0.f : 1.f;
		glVertexAttrib4f(GLA_Diffuse,Value,Value,Value,1.f);
		RenDev->GL.BlackDiffuse = BlackDiffuse;
	}

	if( Indexed && RenDev->GL.ElementBuffer != CurrentState->IndexBuffer )
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,CurrentState->IndexBuffer);
		RenDev->GL.ElementBuffer = CurrentState->IndexBuffer;
	}
	FOpenGLMaterialState& M = CurrentState->Material;
	// Only the perspective world's depth feeds the post process, flat labels and the HUD go to the overlay.
	UBOOL DepthPass = M.DepthPass && InWorld && !S->RenderTarget && S->CameraToScreen.M[2][3] == 1.f;
	// The mask that keeps blended draws off the heat haze in the world's alpha covers every attachment,
	// so a puddle's reflectivity is drawn once more into the reflection's alpha alone.
	UBOOL ReflectivityPass = M.Reflectivity && !RenDev->GL.AlphaMask && RenDev->ScreenSpaceReflections;
	for( INT Pass=0; Pass<3; Pass++ )
	{
		if( Pass == 1 )
		{
			if( !ReflectivityPass )
				continue;
			RenDev->SetWorldDrawBuffers(1);
			glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
		}
		if( Pass == 2 )
		{
			if( ReflectivityPass )
				RenDev->SetWorldDrawBuffers(0);
			if( !DepthPass )
				break;
			// Only the mostly opaque part, the soft edges would hide what draws behind them later.
			Program->SentAlphaRef = Max(M.AlphaRef,0.5f);
			glUniform1f(Program->AlphaRef,Program->SentAlphaRef);
			glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
			glDepthMask(RenDev->GL.DepthMask = GL_TRUE);
		}
		if( Indexed )
		{
			GLenum	Type = CurrentState->IndexSize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
			// Not glDrawRangeElements, ANGLE rejects draws whose indices leave the range and D3D treats it as a hint.
			void*	Indices = (void*)(PTRINT)(FirstIndex * CurrentState->IndexSize);
			if( DrawBase )
				RenDev->DrawElementsBaseVertex(Mode,Count,Type,Indices,DrawBase);
			else
				glDrawElements(Mode,Count,Type,Indices);
		}
		else
			glDrawArrays(Mode,FirstIndex,Count);
	}
	if( DepthPass || ReflectivityPass )
		glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,RenDev->GL.AlphaMask);

	unguard;
}

struct FDeferredIndices : public FIndexBuffer
{
	BYTE*	Data;
	INT		Size,
			IndexSize;

	FDeferredIndices(BYTE* InData,INT InSize,INT InIndexSize) : Data(InData), Size(InSize), IndexSize(InIndexSize) {}
	virtual QWORD GetCacheId() { return 0; }
	virtual INT GetRevision() { return 0; }
	virtual INT GetSize() { return Size; }
	virtual void GetContents(void* Dest) { appMemcpy(Dest,Data,Size); }
	virtual INT GetIndexSize() { return IndexSize; }
};

void FOpenGLRenderInterface::DrawDeferred()
{
	PushState();
	for( INT i=0; i<DeferredDraws.Num(); i++ )
	{
		FDeferredDraw& D = DeferredDraws(i);
		*CurrentState = D.State;
		CurrentState->Material.Beam = 0;
		INT FirstIndex = D.FirstIndex;
		if( D.IndexBytes )
		{
			FDeferredIndices Indices(&DeferredIndices(D.IndexOffset),D.IndexBytes,D.State.IndexSize);
			FirstIndex += SetDynamicIndexBuffer(&Indices,D.State.IndexBufferBase);
		}
		DrawPrimitive(D.Type,FirstIndex,D.NumPrimitives,D.MinIndex,D.MaxIndex);
	}
	PopState();
	DeferredDraws.Empty(DeferredDraws.Num());
	DeferredIndices.Empty(DeferredIndices.Num());
}
