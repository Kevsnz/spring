/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "UnitDrawer.h"
#include "UnitDrawerState.hpp"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/Game.h"
#include "Game/GameHelper.h"
#include "Game/GameSetup.h"
#include "Game/GlobalUnsynced.h"
#include "Game/Players/Player.h"
#include "Game/UI/MiniMap.h"
#include "Map/BaseGroundDrawer.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"

#include "Rendering/Env/ISky.h"
#include "Rendering/Env/IWater.h"
#include "Rendering/Env/CubeMapHandler.h"
#include "Rendering/FarTextureHandler.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/Env/IGroundDecalDrawer.h"
#include "Rendering/Colors.h"
#include "Rendering/IconHandler.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/Textures/3DOTextureHandler.h"
#include "Rendering/Textures/S3OTextureHandler.h"

#include "Sim/Features/Feature.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Projectiles/ExplosionGenerator.h"
#include "Sim/Units/BuildInfo.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"

#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/FileHandler.h"
#include "System/ContainerUtil.h"
#include "System/EventHandler.h"
#include "System/MemPoolTypes.h"
#include "System/SpringMath.h"

#define UNIT_SHADOW_ALPHA_MASKING

CONFIG(int, UnitLodDist).defaultValue(1000).headlessValue(0);
CONFIG(int, UnitIconDist).defaultValue(200).headlessValue(0);
CONFIG(float, UnitIconScaleUI).defaultValue(1.0f).minimumValue(0.5f).maximumValue(2.0f);
CONFIG(float, UnitIconFadeStart).defaultValue(3000.0f).minimumValue(1.0f).maximumValue(10000.0f);
CONFIG(float, UnitIconFadeVanish).defaultValue(1000.0f).minimumValue(1.0f).maximumValue(10000.0f);
CONFIG(float, UnitTransparency).defaultValue(0.7f);
CONFIG(bool, UnitIconsAsUI).defaultValue(false).description("Draw unit icons like it is an UI element and not like unit's LOD.");
CONFIG(bool, UnitIconsHideWithUI).defaultValue(false).description("Hide unit icons when UI is hidden.");

CONFIG(int, MaxDynamicModelLights)
	.defaultValue(1)
	.minimumValue(0);

CONFIG(bool, AdvUnitShading).defaultValue(true).headlessValue(false).safemodeValue(false).description("Determines whether specular highlights and other lighting effects are rendered for units.");




CUnitDrawer* unitDrawer = nullptr;
float CUnitDrawer::iconSizeBase = 32;
float CUnitDrawer::iconScale = 1;
float CUnitDrawer::iconFadeStart = 3000;
float CUnitDrawer::iconFadeVanish = 1000;
float CUnitDrawer::iconZoomDist;

// can not be a CUnitDrawer; destruction in global
// scope might happen after ~EventHandler which is
// referenced by ~EventClient
static uint8_t unitDrawerMem[sizeof(CUnitDrawer)];

static FixedDynMemPool<sizeof(GhostSolidObject), MAX_UNITS / 1000, MAX_UNITS / 32> ghostMemPool;


static void LoadUnitExplosionGenerators() {
	using F = decltype(&UnitDef::AddModelExpGenID);
	using T = decltype(UnitDef::modelCEGTags);

	const auto LoadGenerators = [](UnitDef* ud, const F addExplGenID, const T& explGenTags, const char* explGenPrefix) {
		for (const auto& explGenTag: explGenTags) {
			if (explGenTag[0] == 0)
				break;

			// build a contiguous range of valid ID's
			(ud->*addExplGenID)(explGenHandler.LoadGeneratorID(explGenTag, explGenPrefix));
		}
	};

	for (unsigned int i = 0, n = unitDefHandler->NumUnitDefs(); i < n; i++) {
		UnitDef* ud = const_cast<UnitDef*>(unitDefHandler->GetUnitDefByID(i + 1));

		// piece- and crash-generators can only be custom so the prefix is not required to be given game-side
		LoadGenerators(ud, &UnitDef::AddModelExpGenID, ud->modelCEGTags,                "");
		LoadGenerators(ud, &UnitDef::AddPieceExpGenID, ud->pieceCEGTags, CEG_PREFIX_STRING);
		LoadGenerators(ud, &UnitDef::AddCrashExpGenID, ud->crashCEGTags, CEG_PREFIX_STRING);
	}
}


static const void BindOpaqueTex(const CS3OTextureHandler::S3OTexMat* textureMat) {
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, textureMat->tex2);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureMat->tex1);
}

static const void BindOpaqueTexAtlas(const CS3OTextureHandler::S3OTexMat*) {
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, textureHandler3DO.GetAtlasTex2ID());
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureHandler3DO.GetAtlasTex1ID());
}
static const void BindOpaqueTexDummy(const CS3OTextureHandler::S3OTexMat*) {}

static const void BindShadowTex(const CS3OTextureHandler::S3OTexMat* textureMat) {
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, textureMat->tex2);
}

static const void KillShadowTex(const CS3OTextureHandler::S3OTexMat*) {
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
}


static const void BindShadowTexAtlas(const CS3OTextureHandler::S3OTexMat*) {
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, textureHandler3DO.GetAtlasTex2ID());
}

static const void KillShadowTexAtlas(const CS3OTextureHandler::S3OTexMat*) {
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
}



static const void PushRenderState3DO() {
	BindOpaqueTexAtlas(nullptr);

	glPushAttrib(GL_POLYGON_BIT);
	glDisable(GL_CULL_FACE);
}

static const void PushRenderStateS3O() {
	if (globalRendering->supportRestartPrimitive) {
		glPrimitiveRestartIndexNV(-1U);
	}
}

static const void PushRenderStateASS() { /* no-op */ }

static const void PopRenderState3DO() { glPopAttrib(); }
static const void PopRenderStateS3O() {    /* no-op */ }
static const void PopRenderStateASS() {    /* no-op */ }


static const void SetTeamColorDummy(const IUnitDrawerState* state, int team, const float2 alpha) {}
static const void SetTeamColorValid(const IUnitDrawerState* state, int team, const float2 alpha) { state->SetTeamColor(team, alpha); }



// typedef std::function<void(int)>                                  BindTexFunc;
// typedef std::function<void(const CS3OTextureHandler::S3OTexMat*)> BindTexFunc;
// typedef std::function<void()>                                     KillTexFunc;
typedef const void (*BindTexFunc)(const CS3OTextureHandler::S3OTexMat*);
typedef const void (*BindTexFunc)(const CS3OTextureHandler::S3OTexMat*);
typedef const void (*KillTexFunc)(const CS3OTextureHandler::S3OTexMat*);

typedef const void (*PushRenderStateFunc)();
typedef const void (*PopRenderStateFunc)();

typedef const void (*SetTeamColorFunc)(const IUnitDrawerState*, int team, const float2 alpha);

static const BindTexFunc opaqueTexBindFuncs[MODELTYPE_OTHER] = {
	BindOpaqueTexDummy, // 3DO (no-op, done by PushRenderState3DO)
	BindOpaqueTex,      // S3O
	BindOpaqueTex,      // ASS
};

static const BindTexFunc shadowTexBindFuncs[MODELTYPE_OTHER] = {
	BindShadowTexAtlas, // 3DO
	BindShadowTex,      // S3O
	BindShadowTex,      // ASS
};

static const BindTexFunc* bindModelTexFuncs[] = {
	&opaqueTexBindFuncs[0], // opaque+alpha
	&shadowTexBindFuncs[0], // shadow
};

static const KillTexFunc shadowTexKillFuncs[MODELTYPE_OTHER] = {
	KillShadowTexAtlas, // 3DO
	KillShadowTex,      // S3O
	KillShadowTex,      // ASS
};


static const PushRenderStateFunc renderStatePushFuncs[MODELTYPE_OTHER] = {
	PushRenderState3DO,
	PushRenderStateS3O,
	PushRenderStateASS,
};

static const PopRenderStateFunc renderStatePopFuncs[MODELTYPE_OTHER] = {
	PopRenderState3DO,
	PopRenderStateS3O,
	PopRenderStateASS,
};


static const SetTeamColorFunc setTeamColorFuncs[] = {
	SetTeamColorDummy,
	SetTeamColorValid,
};



// low-level (batch and solo)
// note: also called during SP
void CUnitDrawer::BindModelTypeTexture(int mdlType, int texType) {
	const auto texFun = bindModelTexFuncs[shadowHandler.InShadowPass()][mdlType];
	const auto texMat = textureHandlerS3O.GetTexture(texType);

	texFun(texMat);
}

void CUnitDrawer::PushModelRenderState(int mdlType) { renderStatePushFuncs[mdlType](); }
void CUnitDrawer::PopModelRenderState (int mdlType) { renderStatePopFuncs [mdlType](); }

// mid-level (solo only)
void CUnitDrawer::PushModelRenderState(const S3DModel* m) {
	PushModelRenderState(m->type);
	BindModelTypeTexture(m->type, m->textureType);
}
void CUnitDrawer::PopModelRenderState(const S3DModel* m) {
	PopModelRenderState(m->type);
}

// high-level (solo only)
void CUnitDrawer::PushModelRenderState(const CSolidObject* o) { PushModelRenderState(o->model); }
void CUnitDrawer::PopModelRenderState(const CSolidObject* o) { PopModelRenderState(o->model); }




void CUnitDrawer::InitStatic() {
	if (unitDrawer == nullptr)
		unitDrawer = new (unitDrawerMem) CUnitDrawer();

	unitDrawer->Init();
}
void CUnitDrawer::KillStatic(bool reload) {
	unitDrawer->Kill();

	if (reload)
		return;

	spring::SafeDestruct(unitDrawer);
	memset(unitDrawerMem, 0, sizeof(unitDrawerMem));
}

void CUnitDrawer::Init() {
	eventHandler.AddClient(this);

	LuaObjectDrawer::ReadLODScales(LUAOBJ_UNIT);
	SetUnitDrawDist((float)configHandler->GetInt("UnitLodDist"));
	SetUnitIconDist((float)configHandler->GetInt("UnitIconDist"));
	iconScale = configHandler->GetFloat("UnitIconScaleUI");
	iconFadeStart = configHandler->GetFloat("UnitIconFadeStart");
	iconFadeVanish = configHandler->GetFloat("UnitIconFadeVanish");
	useScreenIcons = configHandler->GetBool("UnitIconsAsUI");
	iconHideWithUI = configHandler->GetBool("UnitIconsHideWithUI");

	alphaValues.x = std::max(0.11f, std::min(1.0f, 1.0f - configHandler->GetFloat("UnitTransparency")));
	alphaValues.y = std::min(1.0f, alphaValues.x + 0.1f);
	alphaValues.z = std::min(1.0f, alphaValues.x + 0.2f);
	alphaValues.w = std::min(1.0f, alphaValues.x + 0.4f);

	LoadUnitExplosionGenerators();

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		opaqueModelRenderers[modelType].Init();
		alphaModelRenderers[modelType].Init();
	}

	unitDefImages.clear();
	unitDefImages.resize(unitDefHandler->NumUnitDefs() + 1);

	deadGhostBuildings.resize(teamHandler.ActiveAllyTeams());
	liveGhostBuildings.resize(teamHandler.ActiveAllyTeams());

	// LH must be initialized before drawer-state is initialized
	lightHandler.Init(2U, configHandler->GetInt("MaxDynamicModelLights"));

	unitDrawerStates.fill(nullptr);
	unitDrawerStates[DRAWER_STATE_SSP] = IUnitDrawerState::GetInstance(globalRendering->haveARB, globalRendering->haveGLSL);
	unitDrawerStates[DRAWER_STATE_FFP] = IUnitDrawerState::GetInstance(                   false,                     false);

	drawModelFuncs[0] = &CUnitDrawer::DrawUnitModelBeingBuiltOpaque;
	drawModelFuncs[1] = &CUnitDrawer::DrawUnitModelBeingBuiltShadow;
	drawModelFuncs[2] = &CUnitDrawer::DrawUnitModel;

	// shared with FeatureDrawer!
	geomBuffer = LuaObjectDrawer::GetGeometryBuffer();

	drawForward = true;
	drawDeferred = (geomBuffer->Valid());
	wireFrameMode = false;

	// NOTE:
	//   advShading can NOT change at runtime if initially false***
	//   (see AdvModelShadingActionExecutor), so we will always use
	//   FFP renderer-state (in ::Draw) in that special case and it
	//   does not matter whether SSP renderer-state is initialized
	//   *** except for DrawAlphaUnits
	advShading = (unitDrawerStates[DRAWER_STATE_SSP]->Init(this) && cubeMapHandler.Init());

	// note: state must be pre-selected before the first drawn frame
	// Sun*Changed can be called first, e.g. if DynamicSun is enabled
	unitDrawerStates[DRAWER_STATE_SEL] = const_cast<IUnitDrawerState*>(GetWantedDrawerState(false));
	iconSizeBase = std::max(16.0f, std::max(globalRendering->viewSizeX, globalRendering->viewSizeY) * iconSizeMult * iconScale);
}

void CUnitDrawer::Kill()
{
	eventHandler.RemoveClient(this);
	autoLinkedEvents.clear();

	unitDrawerStates[DRAWER_STATE_SSP]->Kill(); IUnitDrawerState::FreeInstance(unitDrawerStates[DRAWER_STATE_SSP]);
	unitDrawerStates[DRAWER_STATE_FFP]->Kill(); IUnitDrawerState::FreeInstance(unitDrawerStates[DRAWER_STATE_FFP]);

	cubeMapHandler.Free();

	for (CUnit* u: unsortedUnits) {
		groundDecals->ForceRemoveSolidObject(u);
	}

	for (UnitDefImage& img: unitDefImages) {
		img.Free();
	}

	for (int allyTeam = 0; allyTeam < deadGhostBuildings.size(); ++allyTeam) {
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			auto& lgb = liveGhostBuildings[allyTeam][modelType];
			auto& dgb = deadGhostBuildings[allyTeam][modelType];

			for (auto it = dgb.begin(); it != dgb.end(); ++it) {
				GhostSolidObject* gso = *it;

				if (gso->DecRef())
					continue;

				// <ghost> might be the gbOwner of a decal; groundDecals is deleted after us
				groundDecals->GhostDestroyed(gso);
				ghostMemPool.free(gso);
			}

			dgb.clear();
			lgb.clear();
		}
	}

	// reuse inner vectors when reloading
	// deadGhostBuildings.clear();
	// liveGhostBuildings.clear();


	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		opaqueModelRenderers[modelType].Kill();
		alphaModelRenderers[modelType].Kill();
	}

	unsortedUnits.clear();
	unitsByIcon.clear();

	geomBuffer = nullptr;
}



void CUnitDrawer::Update()
{
	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		UpdateTempDrawUnits(tempOpaqueUnits[modelType]);
		UpdateTempDrawUnits(tempAlphaUnits[modelType]);
	}

	iconUnits.clear();

	const float3 camPos = (camHandler->GetCurrentController()).GetPos();
	const float3 camDir = (camHandler->GetCurrentController()).GetDir();
	float dist = CGround::LineGroundCol(camPos, camDir * 150000.0f, false);
	if (dist < 0)
		dist = std::max(0.0f, CGround::LinePlaneCol(camPos, camDir, 150000.0f, readMap->GetCurrAvgHeight()));
	iconZoomDist = dist;

	for (CUnit* unit: unsortedUnits) {
		if (useScreenIcons)
			UpdateUnitIconStateScreen(unit);
		else
			UpdateUnitIconState(unit);
		UpdateUnitDrawPos(unit);
	}

	if ((useDistToGroundForIcons = (camHandler->GetCurrentController()).GetUseDistToGroundForIcons())) {
		const float3& camPos = camera->GetPos();
		// use the height at the current camera position
		//const float groundHeight = CGround::GetHeightAboveWater(camPos.x, camPos.z, false);
		// use the middle between the highest and lowest position on the map as average
		const float groundHeight = readMap->GetCurrAvgHeight();
		const float overGround = camPos.y - groundHeight;

		sqCamDistToGroundForIcons = overGround * overGround;
	}
}




void CUnitDrawer::Draw(bool drawReflection, bool drawRefraction)
{
	sky->SetupFog();

	assert((CCameraHandler::GetActiveCamera())->GetCamType() != CCamera::CAMTYPE_SHADOW);

	// first do the deferred pass; conditional because
	// most of the water renderers use their own FBO's
	if (drawDeferred && !drawReflection && !drawRefraction)
		LuaObjectDrawer::DrawDeferredPass(LUAOBJ_UNIT);

	// now do the regular forward pass
	if (drawForward)
		DrawOpaquePass(false, drawReflection, drawRefraction);

	farTextureHandler->Draw();

	glDisable(GL_FOG);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_2D);
}

void CUnitDrawer::DrawOpaquePass(bool deferredPass, bool drawReflection, bool drawRefraction)
{
	SetupOpaqueDrawing(deferredPass);

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		PushModelRenderState(modelType);
		DrawOpaqueUnits(modelType, drawReflection, drawRefraction);
		DrawOpaqueAIUnits(modelType);
		PopModelRenderState(modelType);
	}

	ResetOpaqueDrawing(deferredPass);

	// draw all custom'ed units that were bypassed in the loop above
	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_UNIT);
	LuaObjectDrawer::DrawOpaqueMaterialObjects(LUAOBJ_UNIT, deferredPass);
}



void CUnitDrawer::DrawOpaqueUnits(int modelType, bool drawReflection, bool drawRefraction)
{
	const auto& mdlRenderer = opaqueModelRenderers[modelType];
	// const auto& unitBinKeys = mdlRenderer.GetObjectBinKeys();

	for (unsigned int i = 0, n = mdlRenderer.GetNumObjectBins(); i < n; i++) {
		BindModelTypeTexture(modelType, mdlRenderer.GetObjectBinKey(i));

		for (CUnit* unit: mdlRenderer.GetObjectBin(i)) {
			DrawOpaqueUnit(unit, drawReflection, drawRefraction);
		}
	}
}

inline void CUnitDrawer::DrawOpaqueUnit(CUnit* unit, bool drawReflection, bool drawRefraction)
{
	if (!CanDrawOpaqueUnit(unit, drawReflection, drawRefraction))
		return;

	if ((unit->pos).SqDistance(camera->GetPos()) > (unit->sqRadius * unitDrawDistSqr)) {
		farTextureHandler->Queue(unit);
		return;
	}

	if (LuaObjectDrawer::AddOpaqueMaterialObject(unit, LUAOBJ_UNIT))
		return;

	// draw the unit with the default (non-Lua) material
	SetTeamColour(unit->team);
	DrawUnitTrans(unit, 0, 0, false, false);
}


void CUnitDrawer::DrawOpaqueAIUnits(int modelType)
{
	const std::vector<TempDrawUnit>& tmpOpaqueUnits = tempOpaqueUnits[modelType];

	// NOTE: not type-sorted
	for (const TempDrawUnit& unit: tmpOpaqueUnits) {
		if (!camera->InView(unit.pos, 100.0f))
			continue;

		DrawOpaqueAIUnit(unit);
	}
}

void CUnitDrawer::DrawOpaqueAIUnit(const TempDrawUnit& unit)
{
	glPushMatrix();
	glTranslatef3(unit.pos);
	glRotatef(unit.rotation * math::RAD_TO_DEG, 0.0f, 1.0f, 0.0f);

	const UnitDef* def = unit.unitDef;
	const S3DModel* mdl = def->model;

	assert(mdl != nullptr);

	BindModelTypeTexture(mdl->type, mdl->textureType);
	SetTeamColour(unit.team);
	mdl->DrawStatic();

	glPopMatrix();
}


void CUnitDrawer::DrawUnitIcons()
{
	// draw unit icons and radar blips
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.05f);

	// A2C effectiveness is limited below four samples
	if (globalRendering->msaaLevel >= 4)
		glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB);

	for (CUnit* u: iconUnits) {
		const unsigned short closBits = (u->losStatus[gu->myAllyTeam] & (LOS_INLOS                  ));
		const unsigned short plosBits = (u->losStatus[gu->myAllyTeam] & (LOS_PREVLOS | LOS_CONTRADAR));

		DrawIcon(u, !gu->spectatingFullView && closBits == 0 && plosBits != (LOS_PREVLOS | LOS_CONTRADAR));
	}

	glPopAttrib();
}

void CUnitDrawer::DrawUnitIconsScreen()
{
	if (game->hideInterface && iconHideWithUI)
		return;
	
	// draw unit icons and radar blips
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.05f);

	CVertexArray* va = GetVertexArray();
	iconSizeBase = std::max(12.0f, std::max(globalRendering->viewSizeX, globalRendering->viewSizeY) * iconSizeMult * iconScale);

	for (auto iconIt = unitsByIcon.cbegin(); iconIt != unitsByIcon.cend(); ++iconIt)
	{
		const icon::CIconData* icon = iconIt->first;
		const std::vector<const CUnit*>& units = iconIt->second;

		if (icon == nullptr)
			continue;
		if (units.empty())
			continue;

		va->Initialize();
		va->EnlargeArrays(units.size() * 4, 0, VA_SIZE_2DTC);
		icon->BindTexture();

		for (const CUnit* unit: units)
		{
			if (unit->noDraw)
				continue;
			if (unit->IsInVoid())
				continue;
			if (unit->health <= 0 || unit->beingBuilt)
				continue;
			
			const unsigned short closBits = (unit->losStatus[gu->myAllyTeam] & (LOS_INLOS                  ));
			const unsigned short plosBits = (unit->losStatus[gu->myAllyTeam] & (LOS_PREVLOS | LOS_CONTRADAR));

			assert(unit->myIcon == icon);
			DrawIconScreenArray(unit, icon, !gu->spectatingFullView && closBits == 0 && plosBits != (LOS_PREVLOS | LOS_CONTRADAR), iconZoomDist, va);
		}

		va->DrawArray2dTC(GL_QUADS);
	}
	glPopAttrib();
}


/******************************************************************************/
/******************************************************************************/

bool CUnitDrawer::CanDrawOpaqueUnit(
	const CUnit* unit,
	bool drawReflection,
	bool drawRefraction
) const {
	if (unit == (drawReflection? nullptr: (gu->GetMyPlayer())->fpsController.GetControllee()))
		return false;
	if (unit->noDraw)
		return false;
	if (unit->IsInVoid())
		return false;
	// unit will be drawn as icon instead
	if (unit->isIcon)
		return false;

	if (!(unit->losStatus[gu->myAllyTeam] & LOS_INLOS) && !gu->spectatingFullView)
		return false;

	// either PLAYER or UWREFL
	const CCamera* cam = CCameraHandler::GetActiveCamera();

	if (drawRefraction && !unit->IsInWater())
		return false;

	if (drawReflection && !ObjectVisibleReflection(unit->drawMidPos, cam->GetPos(), unit->GetDrawRadius()))
		return false;

	return (cam->InView(unit->drawMidPos, unit->GetDrawRadius()));
}

bool CUnitDrawer::CanDrawOpaqueUnitShadow(const CUnit* unit) const
{
	if (unit->noDraw)
		return false;
	if (unit->IsInVoid())
		return false;
	// no shadow if unit is already an icon from player's POV
	if (unit->isIcon)
		return false;
	if (unit->isCloaked)
		return false;

	const CCamera* cam = CCameraHandler::GetActiveCamera();

	const bool unitInLOS = ((unit->losStatus[gu->myAllyTeam] & LOS_INLOS) || gu->spectatingFullView);
	const bool unitInView = cam->InView(unit->drawMidPos, unit->GetDrawRadius());

	return (unitInLOS && unitInView);
}




void CUnitDrawer::DrawOpaqueUnitShadow(CUnit* unit) {
	if (!CanDrawOpaqueUnitShadow(unit))
		return;

	if (LuaObjectDrawer::AddShadowMaterialObject(unit, LUAOBJ_UNIT))
		return;

	DrawUnitTrans(unit, 0, 0, false, false);
}


void CUnitDrawer::DrawOpaqueUnitsShadow(int modelType) {
	const auto& mdlRenderer = opaqueModelRenderers[modelType];
	// const auto& unitBinKeys = mdlRenderer.GetObjectBinKeys();

	for (unsigned int i = 0, n = mdlRenderer.GetNumObjectBins(); i < n; i++) {
		// only need to bind the atlas once for 3DO's, but KISS
		assert((modelType != MODELTYPE_3DO) || (mdlRenderer.GetObjectBinKey(i) == 0));
		shadowTexBindFuncs[modelType](textureHandlerS3O.GetTexture(mdlRenderer.GetObjectBinKey(i)));

		for (CUnit* unit: mdlRenderer.GetObjectBin(i)) {
			DrawOpaqueUnitShadow(unit);
		}

		shadowTexKillFuncs[modelType](nullptr);
	}
}

void CUnitDrawer::DrawShadowPass()
{
	glColor3f(1.0f, 1.0f, 1.0f);
	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);

	#ifdef UNIT_SHADOW_ALPHA_MASKING
	glAlphaFunc(GL_GREATER, 0.5f);
	glEnable(GL_ALPHA_TEST);
	#endif

	Shader::IProgramObject* po = shadowHandler.GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);
	po->Enable();

	{
		assert((CCameraHandler::GetActiveCamera())->GetCamType() == CCamera::CAMTYPE_SHADOW);

		// 3DO's have clockwise-wound faces and
		// (usually) holes, so disable backface
		// culling for them
		glDisable(GL_CULL_FACE);
		DrawOpaqueUnitsShadow(MODELTYPE_3DO);
		glEnable(GL_CULL_FACE);

		for (int modelType = MODELTYPE_S3O; modelType < MODELTYPE_OTHER; modelType++) {
			// note: just use DrawOpaqueUnits()? would
			// save texture switches needed anyway for
			// UNIT_SHADOW_ALPHA_MASKING
			DrawOpaqueUnitsShadow(modelType);
		}
	}

	po->Disable();

	#ifdef UNIT_SHADOW_ALPHA_MASKING
	glDisable(GL_ALPHA_TEST);
	#endif

	glDisable(GL_POLYGON_OFFSET_FILL);

	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_UNIT);
	LuaObjectDrawer::DrawShadowMaterialObjects(LUAOBJ_UNIT, false);
}

void CUnitDrawer::DrawIconScreenArray(const CUnit* unit, const icon::CIconData* icon, bool useDefaultIcon, const float dist, CVertexArray* va)
{
	// iconUnits should not never contain void-space units, see UpdateUnitIconState
	assert(!unit->IsInVoid());

	// drawMidPos is auto-calculated now; can wobble on its own as pieces move
	float3 pos = (!gu->spectatingFullView) ?
		unit->GetObjDrawErrorPos(gu->myAllyTeam) :
		unit->GetObjDrawMidPos();
	
	pos = camera->CalcWindowCoordinates(pos);
	if (pos.z < 0)
		return;

	// use white for selected units
	const uint8_t* srcColor = unit->isSelected? color4::white: teamHandler.Team(unit->team)->color;
	uint8_t color[4] = { srcColor[0], srcColor[1], srcColor[2], 255 };

	float unitRadiusMult = icon->GetSize();
	if (icon->GetRadiusAdjust() && !useDefaultIcon)
		unitRadiusMult *= (unit->radius / icon->GetRadiusScale());
	unitRadiusMult = (unitRadiusMult - 1) * 0.75 + 1;

	// fade icons away in high zoom in levels
	if (!unit->isIcon)
		if (dist / unitRadiusMult < iconFadeVanish)
			return;
		else if (iconFadeVanish < iconFadeStart && dist / unitRadiusMult < iconFadeStart)
			// alpha range [64, 255], since icons is unrecognisable with alpha < 64
			color[3] = 64 + 191.0f * (dist / unitRadiusMult - iconFadeVanish) / (iconFadeStart - iconFadeVanish);

	// calculate the vertices
	const float offset = iconSizeBase / 2.0f * unitRadiusMult;

	const float x0 = (pos.x - offset) / globalRendering->viewSizeX;
	const float y0 = (pos.y + offset) / globalRendering->viewSizeY;
	const float x1 = (pos.x + offset) / globalRendering->viewSizeX;
	const float y1 = (pos.y - offset) / globalRendering->viewSizeY;

	if (x1 < 0 || x0 > 1 || y0 < 0 || y1 > 1)
		return; // don't try to draw outside the screen

	// Draw the icon.
	icon->DrawArray(va, x0, y0, x1, y1, color);
}

void CUnitDrawer::DrawIcon(CUnit* unit, bool useDefaultIcon)
{
	// iconUnits should not never contain void-space units, see UpdateUnitIconState
	assert(!unit->IsInVoid());

	// If the icon is to be drawn as a radar blip, we want to get the default icon.
	const icon::CIconData* iconData = nullptr;

	if (useDefaultIcon) {
		iconData = icon::iconHandler.GetDefaultIconData();
	} else {
		iconData = unit->unitDef->iconType.GetIconData();
	}

	// drawMidPos is auto-calculated now; can wobble on its own as pieces move
	float3 pos = (!gu->spectatingFullView) ?
		unit->GetObjDrawErrorPos(gu->myAllyTeam) :
		unit->GetObjDrawMidPos();

	// make sure icon is above ground (needed before we calculate scale below)
	const float h = CGround::GetHeightReal(pos.x, pos.z, false);

	pos.y = std::max(pos.y, h);

	// Calculate the icon size. It scales with:
	//  * The square root of the camera distance.
	//  * The mod defined 'iconSize' (which acts a multiplier).
	//  * The unit radius, depending on whether the mod defined 'radiusadjust' is true or false.
	const float dist = std::min(8000.0f, fastmath::sqrt_builtin(camera->GetPos().SqDistance(pos)));
	const float iconScale = 0.4f * fastmath::sqrt_builtin(dist); // makes far icons bigger
	float scale = iconData->GetSize() * iconScale;

	if (iconData->GetRadiusAdjust() && !useDefaultIcon)
		scale *= (unit->radius / iconData->GetRadiusScale());

	// make sure icon is not partly under ground
	pos.y = std::max(pos.y, h + (unit->iconRadius = scale));

	// use white for selected units
	const uint8_t* colors[] = {teamHandler.Team(unit->team)->color, color4::white};
	const uint8_t* color = colors[unit->isSelected];

	glColor3ubv(color);

	// calculate the vertices
	const float3 dy = camera->GetUp()    * scale;
	const float3 dx = camera->GetRight() * scale;
	const float3 vn = pos - dx;
	const float3 vp = pos + dx;
	const float3 vnn = vn - dy;
	const float3 vpn = vp - dy;
	const float3 vnp = vn + dy;
	const float3 vpp = vp + dy;

	// Draw the icon.
	iconData->Draw(vnn, vpn, vnp, vpp);
}






void CUnitDrawer::SetupAlphaDrawing(bool deferredPass)
{
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE * wireFrameMode + GL_FILL * (1 - wireFrameMode));

	unitDrawerStates[DRAWER_STATE_SEL] = const_cast<IUnitDrawerState*>(GetWantedDrawerState(true));
	unitDrawerStates[DRAWER_STATE_SEL]->Enable(this, /*deferredPass*/ false, true);

	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.1f);
	glDepthMask(GL_FALSE);
}

void CUnitDrawer::ResetAlphaDrawing(bool deferredPass)
{
	unitDrawerStates[DRAWER_STATE_SEL]->Disable(this, /*deferredPass*/ false);

	glPopAttrib();
}



void CUnitDrawer::DrawAlphaPass()
{
	{
		SetupAlphaDrawing(false);

		if (UseAdvShading())
			glDisable(GL_ALPHA_TEST);

		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			PushModelRenderState(modelType);
			DrawAlphaUnits(modelType);
			DrawAlphaAIUnits(modelType);
			PopModelRenderState(modelType);
		}

		if (UseAdvShading())
			glEnable(GL_ALPHA_TEST);

		ResetAlphaDrawing(false);
	}

	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_UNIT);
	LuaObjectDrawer::DrawAlphaMaterialObjects(LUAOBJ_UNIT, false);
}

void CUnitDrawer::DrawAlphaUnits(int modelType)
{
	{
		const auto& mdlRenderer = alphaModelRenderers[modelType];
		// const auto& unitBinKeys = mdlRenderer.GetObjectBinKeys();

		for (unsigned int i = 0, n = mdlRenderer.GetNumObjectBins(); i < n; i++) {
			BindModelTypeTexture(modelType, mdlRenderer.GetObjectBinKey(i));

			for (CUnit* unit: mdlRenderer.GetObjectBin(i)) {
				DrawAlphaUnit(unit, modelType, false);
			}
		}
	}

	// living and dead ghosted buildings
	if (!gu->spectatingFullView)
		DrawGhostedBuildings(modelType);
}

inline void CUnitDrawer::DrawAlphaUnit(CUnit* unit, int modelType, bool drawGhostBuildingsPass) {
	if (!camera->InView(unit->drawMidPos, unit->GetDrawRadius()))
		return;

	if (LuaObjectDrawer::AddAlphaMaterialObject(unit, LUAOBJ_UNIT))
		return;

	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	if (drawGhostBuildingsPass) {
		// check for decoy models
		const UnitDef* decoyDef = unit->unitDef->decoyDef;
		const S3DModel* model = nullptr;

		if (decoyDef == nullptr) {
			model = unit->model;
		} else {
			model = decoyDef->LoadModel();
		}

		// FIXME: needs a second pass
		if (model->type != modelType)
			return;

		// ghosted enemy units
		if (losStatus & LOS_CONTRADAR) {
			glColor4f(0.9f, 0.9f, 0.9f, alphaValues.z);
		} else {
			glColor4f(0.6f, 0.6f, 0.6f, alphaValues.y);
		}

		glPushMatrix();
		glTranslatef3(unit->drawPos);
		glRotatef(unit->buildFacing * 90.0f, 0, 1, 0);

		// the units in liveGhostedBuildings[modelType] are not
		// sorted by textureType, but we cannot merge them with
		// alphaModelRenderers[modelType] either since they are
		// not actually cloaked
		BindModelTypeTexture(modelType, model->textureType);

		SetTeamColour(unit->team, float2((losStatus & LOS_CONTRADAR)? alphaValues.z: alphaValues.y, 1.0f));
		model->DrawStatic();
		glPopMatrix();

		glColor4f(1.0f, 1.0f, 1.0f, alphaValues.x);
		return;
	}

	if (unit->isIcon)
		return;

	if ((losStatus & LOS_INLOS) || gu->spectatingFullView) {
		SetTeamColour(unit->team, float2(alphaValues.x, 1.0f));
		DrawUnitTrans(unit, 0, 0, false, false);
	}
}



void CUnitDrawer::DrawAlphaAIUnits(int modelType)
{
	std::vector<TempDrawUnit>& tmpAlphaUnits = tempAlphaUnits[modelType];

	// NOTE: not type-sorted
	for (const TempDrawUnit& unit: tmpAlphaUnits) {
		if (!camera->InView(unit.pos, 100.0f))
			continue;

		DrawAlphaAIUnit(unit);
		DrawAlphaAIUnitBorder(unit);
	}
}

void CUnitDrawer::DrawAlphaAIUnit(const TempDrawUnit& unit)
{
	glPushMatrix();
	glTranslatef3(unit.pos);
	glRotatef(unit.rotation * math::RAD_TO_DEG, 0.0f, 1.0f, 0.0f);

	const UnitDef* def = unit.unitDef;
	const S3DModel* mdl = def->model;

	assert(mdl != nullptr);

	BindModelTypeTexture(mdl->type, mdl->textureType);
	SetTeamColour(unit.team, float2(alphaValues.x, 1.0f));
	mdl->DrawStatic();

	glPopMatrix();
}

void CUnitDrawer::DrawAlphaAIUnitBorder(const TempDrawUnit& unit)
{
	if (!unit.drawBorder)
		return;

	SetTeamColour(unit.team, float2(alphaValues.w, 1.0f));

	const BuildInfo buildInfo(unit.unitDef, unit.pos, unit.facing);
	const float3 buildPos = CGameHelper::Pos2BuildPos(buildInfo, false);

	const float xsize = buildInfo.GetXSize() * (SQUARE_SIZE >> 1);
	const float zsize = buildInfo.GetZSize() * (SQUARE_SIZE >> 1);

	glColor4f(0.2f, 1, 0.2f, alphaValues.w);
	glDisable(GL_TEXTURE_2D);
	glBegin(GL_LINE_STRIP);
		glVertexf3(buildPos + float3( xsize, 1.0f,  zsize));
		glVertexf3(buildPos + float3(-xsize, 1.0f,  zsize));
		glVertexf3(buildPos + float3(-xsize, 1.0f, -zsize));
		glVertexf3(buildPos + float3( xsize, 1.0f, -zsize));
		glVertexf3(buildPos + float3( xsize, 1.0f,  zsize));
	glEnd();
	glColor4f(1.0f, 1.0f, 1.0f, alphaValues.x);
	glEnable(GL_TEXTURE_2D);
}

void CUnitDrawer::UpdateGhostedBuildings()
{
	for (int allyTeam = 0; allyTeam < deadGhostBuildings.size(); ++allyTeam) {
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			auto& dgb = deadGhostBuildings[allyTeam][modelType];

			for (int i = 0; i < dgb.size(); /*no-op*/) {
				GhostSolidObject* gso = dgb[i];

				if (!losHandler->InLos(gso->pos, allyTeam)) {
					++i;
					continue;
				}

				// obtained LOS on the ghost of a dead building
				if (!gso->DecRef()) {
					groundDecals->GhostDestroyed(gso);
					ghostMemPool.free(gso);
				}

				dgb[i] = dgb.back();
				dgb.pop_back();
			}
		}
	}
}

void CUnitDrawer::DrawGhostedBuildings(int modelType)
{
	assert((unsigned) gu->myAllyTeam < deadGhostBuildings.size());

	std::vector<GhostSolidObject*>& deadGhostedBuildings = deadGhostBuildings[gu->myAllyTeam][modelType];
	std::vector<CUnit*>& liveGhostedBuildings = liveGhostBuildings[gu->myAllyTeam][modelType];

	glColor4f(0.6f, 0.6f, 0.6f, alphaValues.y);

	// buildings that died while ghosted
	for (auto it = deadGhostedBuildings.begin(); it != deadGhostedBuildings.end(); ++it) {
		if (camera->InView((*it)->pos, (*it)->model->GetDrawRadius())) {
			glPushMatrix();
			glTranslatef3((*it)->pos);
			glRotatef((*it)->facing * 90.0f, 0, 1, 0);

			BindModelTypeTexture(modelType, (*it)->model->textureType);
			SetTeamColour((*it)->team, float2(alphaValues.y, 1.0f));

			(*it)->model->DrawStatic();
			glPopMatrix();
			(*it)->lastDrawFrame = globalRendering->drawFrame;
		}
	}

	for (CUnit* u: liveGhostedBuildings) {
		DrawAlphaUnit(u, modelType, true);
	}
}






void CUnitDrawer::SetupOpaqueDrawing(bool deferredPass)
{
	glPushAttrib(GL_ENABLE_BIT | GL_POLYGON_BIT);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE * wireFrameMode + GL_FILL * (1 - wireFrameMode));

	glCullFace(GL_BACK);
	glEnable(GL_CULL_FACE);

	glAlphaFunc(GL_GREATER, 0.5f);
	glEnable(GL_ALPHA_TEST);

	// pick base shaders (ARB/GLSL) or FFP; not used by custom-material models
	unitDrawerStates[DRAWER_STATE_SEL] = const_cast<IUnitDrawerState*>(GetWantedDrawerState(false));
	unitDrawerStates[DRAWER_STATE_SEL]->Enable(this, deferredPass, false);

	// NOTE:
	//   when deferredPass is true we MUST be able to use the SSP render-state
	//   all calling code (reached from DrawOpaquePass(deferred=true)) should
	//   ensure this is the case
	assert(!deferredPass || advShading);
	assert(!deferredPass || unitDrawerStates[DRAWER_STATE_SEL]->CanDrawDeferred());
}

void CUnitDrawer::ResetOpaqueDrawing(bool deferredPass)
{
	unitDrawerStates[DRAWER_STATE_SEL]->Disable(this, deferredPass);

	glPopAttrib();
}

const IUnitDrawerState* CUnitDrawer::GetWantedDrawerState(bool alphaPass) const
{
	// proper alpha-rendering is only enabled with GLSL state
	// (ARB shaders could technically also be used, but KISS)
	const bool enableShaders =               unitDrawerStates[DRAWER_STATE_SSP]->CanEnable(this);
	const bool permitShaders = !alphaPass || unitDrawerStates[DRAWER_STATE_SSP]->CanDrawAlpha();

	return unitDrawerStates[enableShaders && permitShaders];
}



void CUnitDrawer::SetTeamColour(int team, const float2 alpha) const
{
	// need this because we can be called by no-team projectiles
	const int b0 = teamHandler.IsValidTeam(team);
	// should be an assert, but projectiles (+FlyingPiece) would trigger it
	const int b1 = !shadowHandler.InShadowPass();

	setTeamColorFuncs[b0 * b1](unitDrawerStates[DRAWER_STATE_SEL], team, alpha);
}

/**
 * Set up the texture environment in texture unit 0
 * to give an S3O texture its team-colour.
 *
 * Also:
 * - call SetBasicTeamColour to set the team colour to transform to.
 * - Replace the output alpha channel. If not, only the team-coloured bits will show, if that. Or something.
 */
void CUnitDrawer::SetupBasicS3OTexture0()
{
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);

	// RGB = Texture * (1 - Alpha) + Teamcolor * Alpha
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_INTERPOLATE_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_CONSTANT_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB_ARB, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

	// ALPHA = Ignore
}

/**
 * This sets the first texture unit to GL_MODULATE the colours from the
 * first texture unit with the current glColor.
 *
 * Normal S3O drawing sets the color to full white; translucencies
 * use this setup to 'tint' the drawn model.
 *
 * - Leaves glActivateTextureARB at the first unit.
 * - This doesn't tinker with the output alpha, either.
 */
void CUnitDrawer::SetupBasicS3OTexture1()
{
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);

	// RGB = Primary Color * Previous
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_PRIMARY_COLOR_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);

	// ALPHA = Current alpha * Alpha mask
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA_ARB, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_ARB, GL_PRIMARY_COLOR_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA_ARB, GL_SRC_ALPHA);
}


void CUnitDrawer::CleanupBasicS3OTexture1()
{
	// reset texture1 state
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE1_ALPHA_ARB, GL_PREVIOUS_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE0_RGB_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
}

void CUnitDrawer::CleanupBasicS3OTexture0()
{
	// reset texture0 state
	glActiveTexture(GL_TEXTURE0);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE2_RGB_ARB, GL_CONSTANT_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_OPERAND2_RGB_ARB, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_RGB_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
}




void CUnitDrawer::PushIndividualOpaqueState(const S3DModel* model, int teamID, bool deferredPass)
{
	// these are not handled by Setup*Drawing but CGame
	// easier to assume they no longer have the correct
	// values at this point
	glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	SetupOpaqueDrawing(deferredPass);
	PushModelRenderState(model);
	SetTeamColour(teamID);
}

void CUnitDrawer::PushIndividualAlphaState(const S3DModel* model, int teamID, bool deferredPass)
{
	SetupAlphaDrawing(deferredPass);
	PushModelRenderState(model);
	SetTeamColour(teamID, float2(alphaValues.x, 1.0f));
}


void CUnitDrawer::PopIndividualOpaqueState(const S3DModel* model, int teamID, bool deferredPass)
{
	PopModelRenderState(model);
	ResetOpaqueDrawing(deferredPass);

	glPopAttrib();
}

void CUnitDrawer::PopIndividualAlphaState(const S3DModel* model, int teamID, bool deferredPass)
{
	PopModelRenderState(model);
	ResetAlphaDrawing(deferredPass);
}


void CUnitDrawer::PushIndividualOpaqueState(const CUnit* unit, bool deferredPass)
{
	PushIndividualOpaqueState(unit->model, unit->team, deferredPass);
}

void CUnitDrawer::PopIndividualOpaqueState(const CUnit* unit, bool deferredPass)
{
	PopIndividualOpaqueState(unit->model, unit->team, deferredPass);
}


void CUnitDrawer::DrawIndividual(const CUnit* unit, bool noLuaCall)
{
	if (LuaObjectDrawer::DrawSingleObject(unit, LUAOBJ_UNIT /*, noLuaCall*/))
		return;

	// set the full default state
	PushIndividualOpaqueState(unit, false);
	DrawUnitTrans(unit, 0, 0, false, noLuaCall);
	PopIndividualOpaqueState(unit, false);
}

void CUnitDrawer::DrawIndividualNoTrans(const CUnit* unit, bool noLuaCall)
{
	if (LuaObjectDrawer::DrawSingleObjectNoTrans(unit, LUAOBJ_UNIT /*, noLuaCall*/))
		return;

	PushIndividualOpaqueState(unit, false);
	DrawUnitNoTrans(unit, 0, 0, false, noLuaCall);
	PopIndividualOpaqueState(unit, false);
}




static void DIDResetPrevProjection(bool toScreen)
{
	if (!toScreen)
		return;

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPushMatrix();
}

static void DIDResetPrevModelView()
{
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glPushMatrix();
}

static bool DIDCheckMatrixMode(int wantedMode)
{
	#if 1
	int matrixMode = 0;
	glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
	return (matrixMode == wantedMode);
	#else
	return true;
	#endif
}


// used by LuaOpenGL::Draw{Unit,Feature}Shape
// acts like DrawIndividual but can not apply
// custom materials
void CUnitDrawer::DrawIndividualDefOpaque(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen)
{
	const S3DModel* model = objectDef->LoadModel();

	if (model == nullptr)
		return;

	if (!rawState) {
		if (!DIDCheckMatrixMode(GL_MODELVIEW))
			return;

		// teamID validity is checked by SetTeamColour
		unitDrawer->PushIndividualOpaqueState(model, teamID, false);

		// NOTE:
		//   unlike DrawIndividual(...) the model transform is
		//   always provided by Lua, not taken from the object
		//   (which does not exist here) so we must restore it
		//   (by undoing the UnitDrawerState MVP setup)
		//
		//   assumes the Lua transform includes a LoadIdentity!
		DIDResetPrevProjection(toScreen);
		DIDResetPrevModelView();
	}

	model->DrawStatic();

	if (!rawState) {
		unitDrawer->PopIndividualOpaqueState(model, teamID, false);
	}
}

// used for drawing building orders (with translucency)
void CUnitDrawer::DrawIndividualDefAlpha(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen)
{
	const S3DModel* model = objectDef->LoadModel();

	if (model == nullptr)
		return;

	if (!rawState) {
		if (!DIDCheckMatrixMode(GL_MODELVIEW))
			return;

		unitDrawer->PushIndividualAlphaState(model, teamID, false);

		DIDResetPrevProjection(toScreen);
		DIDResetPrevModelView();
	}

	model->DrawStatic();

	if (!rawState) {
		unitDrawer->PopIndividualAlphaState(model, teamID, false);
	}
}






typedef const void (*DrawModelBuildStageFunc)(const CUnit*, const double*, const double*, bool);

static const void DrawModelNoopBuildStageOpaque(const CUnit*, const double*, const double*, bool) {}
static const void DrawModelNoopBuildStageShadow(const CUnit*, const double*, const double*, bool) {}

static const void DrawModelWireBuildStageOpaque(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glClipPlane(GL_CLIP_PLANE0, upperPlane);
	glClipPlane(GL_CLIP_PLANE1, lowerPlane);

	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

static const void DrawModelWireBuildStageOpaqueATI(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	// some ATi mobility cards/drivers dont like clipping wireframes
	glDisable(GL_CLIP_PLANE0);
	glDisable(GL_CLIP_PLANE1);

	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);
}


static const void DrawModelFlatBuildStageOpaque(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glClipPlane(GL_CLIP_PLANE0, upperPlane);
	glClipPlane(GL_CLIP_PLANE1, lowerPlane);

	CUnitDrawer::DrawUnitModel(unit, noLuaCall);
}


static const void DrawModelFillBuildStageOpaque(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glClipPlane(GL_CLIP_PLANE0, upperPlane);

	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glDisable(GL_POLYGON_OFFSET_FILL);
}

static const void DrawModelFillBuildStageOpaqueATI(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glDisable(GL_CLIP_PLANE0);

	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glDisable(GL_POLYGON_OFFSET_FILL);
}




static const void DrawModelWireBuildStageShadow(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glPushMatrix();
	glLoadIdentity();
	glClipPlane(GL_CLIP_PLANE0, upperPlane);
	glClipPlane(GL_CLIP_PLANE1, lowerPlane);
	glPopMatrix();

	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

static const void DrawModelWireBuildStageShadowATI(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glDisable(GL_CLIP_PLANE0);
	glDisable(GL_CLIP_PLANE1);

	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);
}


static const void DrawModelFlatBuildStageShadow(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	glPushMatrix();
	glLoadIdentity();
	glClipPlane(GL_CLIP_PLANE0, upperPlane);
	glClipPlane(GL_CLIP_PLANE1, lowerPlane);
	glPopMatrix();

	CUnitDrawer::DrawUnitModel(unit, noLuaCall);
}


static const void DrawModelFillBuildStageShadow(
	const CUnit* unit,
	const double* upperPlane,
	const double* lowerPlane,
	bool noLuaCall
) {
	CUnitDrawer::DrawUnitModel(unit, noLuaCall);
}


static constexpr DrawModelBuildStageFunc drawModelBuildStageOpaqueFuncs[4 + 4] = {
	// amdHacks=0
	DrawModelNoopBuildStageOpaque,
	DrawModelWireBuildStageOpaque,
	DrawModelFlatBuildStageOpaque,
	DrawModelFillBuildStageOpaque,
	// amdHacks=1
	DrawModelNoopBuildStageOpaque,
	DrawModelWireBuildStageOpaqueATI,
	DrawModelFlatBuildStageOpaque,
	DrawModelFillBuildStageOpaqueATI,
};

static constexpr DrawModelBuildStageFunc drawModelBuildStageShadowFuncs[4 + 4] = {
	// amdHacks=0
	DrawModelNoopBuildStageShadow,
	DrawModelWireBuildStageShadow,
	DrawModelFlatBuildStageShadow,
	DrawModelFillBuildStageShadow,
	// amdHacks=1
	DrawModelNoopBuildStageShadow,
	DrawModelWireBuildStageShadowATI,
	DrawModelFlatBuildStageShadow,
	DrawModelFillBuildStageShadow,
};

enum {
	BUILDSTAGE_WIRE = 0,
	BUILDSTAGE_FLAT = 1,
	BUILDSTAGE_FILL = 2,
	BUILDSTAGE_NONE = 3,
};




void CUnitDrawer::DrawUnitModelBeingBuiltShadow(const CUnit* unit, bool noLuaCall)
{
	const float3 stageBounds = {0.0f, unit->model->CalcDrawHeight(), unit->buildProgress};

	// draw-height defaults to maxs.y - mins.y, but can be overridden for non-3DO models
	// the default value derives from the model vertices and makes more sense to use here
	//
	// Both clip planes move up. Clip plane 0 is the upper bound of the model,
	// clip plane 1 is the lower bound. In other words, clip plane 0 makes the
	// wireframe/flat color/texture appear, and clip plane 1 then erases the
	// wireframe/flat color later on.
	const double upperPlanes[4][4] = {
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f       )},
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f - 1.0f)},
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f - 2.0f)},
		{0.0f,  0.0f, 0.0f,                                                          0.0f },
	};
	const double lowerPlanes[4][4] = {
		{0.0f,  1.0f, 0.0f, -stageBounds.x - stageBounds.y * (stageBounds.z * 10.0f - 9.0f)},
		{0.0f,  1.0f, 0.0f, -stageBounds.x - stageBounds.y * (stageBounds.z *  3.0f - 2.0f)},
		{0.0f,  1.0f, 0.0f,                                  (                        0.0f)},
		{0.0f,  0.0f, 0.0f,                                                           0.0f },
	};

	DrawModelBuildStageFunc stageFunc = nullptr;
	// Shader::IProgramObject* shadowProg = shadowHandler.GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);

	glPushAttrib(GL_CURRENT_BIT);
	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);

	{
		// wireframe, unconditional
		stageFunc = drawModelBuildStageShadowFuncs[(globalRendering->amdHacks * 4) + (BUILDSTAGE_WIRE + 1) * (stageBounds.z > 0.000f)];
		stageFunc(unit, upperPlanes[BUILDSTAGE_WIRE], lowerPlanes[BUILDSTAGE_WIRE], noLuaCall);
	}
	{
		// flat-colored, conditional
		stageFunc = drawModelBuildStageShadowFuncs[(globalRendering->amdHacks * 4) + (BUILDSTAGE_FLAT + 1) * (stageBounds.z > 0.333f)];
		stageFunc(unit, upperPlanes[BUILDSTAGE_FLAT], lowerPlanes[BUILDSTAGE_FLAT], noLuaCall);
	}

	glDisable(GL_CLIP_PLANE1);
	glDisable(GL_CLIP_PLANE0);

	{
		// fully-shaded, conditional
		stageFunc = drawModelBuildStageShadowFuncs[(globalRendering->amdHacks * 4) + (BUILDSTAGE_FILL + 1) * (stageBounds.z > 0.666f)];
		stageFunc(unit, upperPlanes[BUILDSTAGE_FILL], lowerPlanes[BUILDSTAGE_FILL], noLuaCall);
	}

	glPopAttrib();
}

void CUnitDrawer::DrawUnitModelBeingBuiltOpaque(const CUnit* unit, bool noLuaCall)
{
	const S3DModel* model = unit->model;
	const    CTeam*  team = teamHandler.Team(unit->team);
	const   SColor  color = team->color;

	const float wireColorMult = std::fabs(128.0f - ((gs->frameNum * 4) & 255)) / 255.0f + 0.5f;
	const float flatColorMult = 1.5f - wireColorMult;

	const float3 frameColors[2] = {unit->unitDef->nanoColor, {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f}};
	const float3 stageColors[2] = {frameColors[globalRendering->teamNanospray], frameColors[globalRendering->teamNanospray]};
	const float3 stageBounds    = {0.0f, model->CalcDrawHeight(), unit->buildProgress};

	// draw-height defaults to maxs.y - mins.y, but can be overridden for non-3DO models
	// the default value derives from the model vertices and makes more sense to use here
	//
	// Both clip planes move up. Clip plane 0 is the upper bound of the model,
	// clip plane 1 is the lower bound. In other words, clip plane 0 makes the
	// wireframe/flat color/texture appear, and clip plane 1 then erases the
	// wireframe/flat color later on.
	const double upperPlanes[4][4] = {
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f       )},
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f - 1.0f)},
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f - 2.0f)},
		{0.0f,  0.0f, 0.0f,                                                          0.0f },
	};
	const double lowerPlanes[4][4] = {
		{0.0f,  1.0f, 0.0f, -stageBounds.x - stageBounds.y * (stageBounds.z * 10.0f - 9.0f)},
		{0.0f,  1.0f, 0.0f, -stageBounds.x - stageBounds.y * (stageBounds.z *  3.0f - 2.0f)},
		{0.0f,  1.0f, 0.0f,                                  (                        0.0f)},
		{0.0f,  0.0f, 0.0f,                                                           0.0f },
	};

	// note: draw-func for stage i is at index i+1 (noop-func is at 0)
	DrawModelBuildStageFunc stageFunc = nullptr;
	IUnitDrawerState* selState = unitDrawer->GetDrawerState(DRAWER_STATE_SEL);

	glPushAttrib(GL_CURRENT_BIT);
	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);

	// wireframe, unconditional
	selState->SetNanoColor(float4(stageColors[0] * wireColorMult, 1.0f));
	stageFunc = drawModelBuildStageOpaqueFuncs[(globalRendering->amdHacks * 4) + (BUILDSTAGE_WIRE + 1) * (stageBounds.z > 0.000f)];
	stageFunc(unit, upperPlanes[BUILDSTAGE_WIRE], lowerPlanes[BUILDSTAGE_WIRE], noLuaCall);

	// flat-colored, conditional
	selState->SetNanoColor(float4(stageColors[1] * flatColorMult, 1.0f));
	stageFunc = drawModelBuildStageOpaqueFuncs[(globalRendering->amdHacks * 4) + (BUILDSTAGE_FLAT + 1) * (stageBounds.z > 0.333f)];
	stageFunc(unit, upperPlanes[BUILDSTAGE_FLAT], lowerPlanes[BUILDSTAGE_FLAT], noLuaCall);

	glDisable(GL_CLIP_PLANE1);

	// fully-shaded, conditional
	selState->SetNanoColor(float4(1.0f, 1.0f, 1.0f, 0.0f)); // turn off
	stageFunc = drawModelBuildStageOpaqueFuncs[(globalRendering->amdHacks * 4) + (BUILDSTAGE_FILL + 1) * (stageBounds.z > 0.666f)];
	stageFunc(unit, upperPlanes[BUILDSTAGE_FILL], lowerPlanes[BUILDSTAGE_FILL], noLuaCall);

	glDisable(GL_CLIP_PLANE0);
	glPopAttrib();
}




void CUnitDrawer::DrawUnitModel(const CUnit* unit, bool noLuaCall) {
	if (!noLuaCall && unit->luaDraw && eventHandler.DrawUnit(unit))
		return;

	unit->localModel.Draw();
}


void CUnitDrawer::DrawUnitNoTrans(
	const CUnit* unit,
	unsigned int preList,
	unsigned int postList,
	bool lodCall,
	bool noLuaCall
) {
	const unsigned int noNanoDraw = lodCall || !unit->beingBuilt || !unit->unitDef->showNanoFrame;
	const unsigned int shadowPass = shadowHandler.InShadowPass();

	if (preList != 0) {
		glCallList(preList);
	}

	// if called from LuaObjectDrawer, unit has a custom material
	//
	// we want Lua-material shaders to have full control over build
	// visualisation, so keep it simple and make LOD-calls draw the
	// full model
	//
	// NOTE: "raw" calls will no longer skip DrawUnitBeingBuilt
	//
	drawModelFuncs[ std::max(noNanoDraw * 2, shadowPass) ](unit, noLuaCall);

	if (postList != 0) {
		glCallList(postList);
	}
}

void CUnitDrawer::DrawUnitTrans(const CUnit* unit, unsigned int preList, unsigned int postList, bool lodCall, bool noLuaCall)
{
	glPushMatrix();
	glMultMatrixf(unit->GetTransformMatrix());

	DrawUnitNoTrans(unit, preList, postList, lodCall, noLuaCall);

	glPopMatrix();
}




inline void CUnitDrawer::UpdateUnitIconState(CUnit* unit) {
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	// reset
	unit->isIcon = losStatus & LOS_INRADAR;

	if ((losStatus & LOS_INLOS) || gu->spectatingFullView)
		unit->isIcon = DrawAsIcon(unit, (unit->pos - camera->GetPos()).SqLength());

	if (!unit->isIcon)
		return;
	if (unit->noDraw)
		return;
	if (unit->IsInVoid())
		return;
	// drawing icons is cheap but not free, avoid a perf-hit when many are offscreen
	if (!camera->InView(unit->drawMidPos, unit->GetDrawRadius()))
		return;

	iconUnits.push_back(unit);
}

inline void CUnitDrawer::UpdateUnitIconStateScreen(CUnit* unit)
{
	if (game->hideInterface && iconHideWithUI) // icons are hidden with UI
	{
		unit->isIcon = false; // draw unit model always
		return;
	}

	if (unit->health <= 0 || unit->beingBuilt)
	{
		unit->isIcon = false;
		return;
	}

	// If the icon is to be drawn as a radar blip, we want to get the default icon.
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];
	const unsigned short plosBits = (losStatus & (LOS_PREVLOS | LOS_CONTRADAR));
	bool useDefaultIcon = !gu->spectatingFullView && !(losStatus & (LOS_INLOS)) && plosBits != (LOS_PREVLOS | LOS_CONTRADAR);

	const icon::CIconData* iconData = useDefaultIcon ? icon::iconHandler.GetDefaultIconData() : unit->unitDef->iconType.GetIconData();

	float iconSizeMult = iconData->GetSize();
	if (iconData->GetRadiusAdjust() && !useDefaultIcon)
		iconSizeMult *= (unit->radius / iconData->GetRadiusScale());
	iconSizeMult = (iconSizeMult - 1) * 0.75 + 1;

	float limit = iconSizeBase/2 * iconSizeMult;

	// calculate unit's radius in screen space and compare with the size of the icon
	float3 pos = unit->pos;
	float3 radiusPos = pos + camera->right * unit->radius;

	pos = camera->CalcWindowCoordinates(pos);
	radiusPos = camera->CalcWindowCoordinates(radiusPos);

	unit->iconRadius = unit->radius * ( (limit * 0.9) / std::abs(pos.x-radiusPos.x) ); // used for clicking on iconified units (world space!!!)

	if (!(losStatus & LOS_INLOS) && !gu->spectatingFullView) // no LOS on unit
	{
		unit->isIcon = losStatus & LOS_INRADAR; // draw icon if unit is on radar
		return;
	}

	// don't render unit's model if it is smaller than icon by 10% in screen space
	// render it anyway in case icon isn't completely opaque (below FadeStart distance)
	unit->isIcon = iconZoomDist/iconSizeMult > iconFadeStart && std::abs(pos.x-radiusPos.x) < limit * 0.9;
}

inline void CUnitDrawer::UpdateUnitDrawPos(CUnit* u) {
	const CUnit* t = u->GetTransporter();

	if (t != nullptr) {
		u->drawPos = u->preFramePos + t->GetDrawDeltaPos(globalRendering->timeOffset);
	} else {
		u->drawPos = u->preFramePos + u->GetDrawDeltaPos(globalRendering->timeOffset);
	}

	u->drawMidPos = u->GetMdlDrawMidPos();
}



bool CUnitDrawer::DrawAsIcon(const CUnit* unit, const float sqUnitCamDist) const {

	const float sqIconDistMult = unit->unitDef->iconType->GetDistanceSqr();
	const float realIconLength = iconLength * sqIconDistMult;

	if (useDistToGroundForIcons)
		return (sqCamDistToGroundForIcons > realIconLength);
	else
		return (sqUnitCamDist > realIconLength);
}


// visualize if a unit can be built at specified position
bool CUnitDrawer::ShowUnitBuildSquare(const BuildInfo& buildInfo)
{
	return ShowUnitBuildSquare(buildInfo, std::vector<Command>());
}

bool CUnitDrawer::ShowUnitBuildSquare(const BuildInfo& buildInfo, const std::vector<Command>& commands)
{
	//TODO: make this a lua callin!
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_TEXTURE_2D);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	CFeature* feature = nullptr;

	std::vector<float3> buildableSquares; // buildable squares
	std::vector<float3> featureSquares; // occupied squares
	std::vector<float3> illegalSquares; // non-buildable squares

	const float3& pos = buildInfo.pos;
	const int x1 = pos.x - (buildInfo.GetXSize() * 0.5f * SQUARE_SIZE);
	const int x2 =    x1 + (buildInfo.GetXSize() *        SQUARE_SIZE);
	const int z1 = pos.z - (buildInfo.GetZSize() * 0.5f * SQUARE_SIZE);
	const int z2 =    z1 + (buildInfo.GetZSize() *        SQUARE_SIZE);
	const float h = CGameHelper::GetBuildHeight(pos, buildInfo.def, false);

	const bool canBuild = !!CGameHelper::TestUnitBuildSquare(
		buildInfo,
		feature,
		-1,
		false,
		&buildableSquares,
		&featureSquares,
		&illegalSquares,
		&commands
	);

	if (canBuild) {
		glColor4f(0.0f, 0.9f, 0.0f, 0.7f);
	} else {
		glColor4f(0.9f, 0.8f, 0.0f, 0.7f);
	}

	CVertexArray* va = GetVertexArray();
	va->Initialize();
	va->EnlargeArrays(buildableSquares.size() * 4, 0, VA_SIZE_0);

	for (unsigned int i = 0; i < buildableSquares.size(); i++) {
		va->AddVertexQ0(buildableSquares[i]                                      );
		va->AddVertexQ0(buildableSquares[i] + float3(SQUARE_SIZE, 0,           0));
		va->AddVertexQ0(buildableSquares[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE));
		va->AddVertexQ0(buildableSquares[i] + float3(          0, 0, SQUARE_SIZE));
	}
	va->DrawArray0(GL_QUADS);


	glColor4f(0.9f, 0.8f, 0.0f, 0.7f);
	va = GetVertexArray();
	va->Initialize();
	va->EnlargeArrays(featureSquares.size() * 4, 0, VA_SIZE_0);

	for (unsigned int i = 0; i < featureSquares.size(); i++) {
		va->AddVertexQ0(featureSquares[i]                                      );
		va->AddVertexQ0(featureSquares[i] + float3(SQUARE_SIZE, 0,           0));
		va->AddVertexQ0(featureSquares[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE));
		va->AddVertexQ0(featureSquares[i] + float3(          0, 0, SQUARE_SIZE));
	}
	va->DrawArray0(GL_QUADS);


	glColor4f(0.9f, 0.0f, 0.0f, 0.7f);
	va = GetVertexArray();
	va->Initialize();
	va->EnlargeArrays(illegalSquares.size() * 4, 0, VA_SIZE_0);

	for (unsigned int i = 0; i < illegalSquares.size(); i++) {
		va->AddVertexQ0(illegalSquares[i]);
		va->AddVertexQ0(illegalSquares[i] + float3(SQUARE_SIZE, 0,           0));
		va->AddVertexQ0(illegalSquares[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE));
		va->AddVertexQ0(illegalSquares[i] + float3(          0, 0, SQUARE_SIZE));
	}
	va->DrawArray0(GL_QUADS);


	if (h < 0.0f) {
		const unsigned char s[4] = { 0,   0, 255, 128 }; // start color
		const unsigned char e[4] = { 0, 128, 255, 255 }; // end color

		va = GetVertexArray();
		va->Initialize();
		va->EnlargeArrays(8, 0, VA_SIZE_C);
		va->AddVertexQC(float3(x1, h, z1), s); va->AddVertexQC(float3(x1, 0.f, z1), e);
		va->AddVertexQC(float3(x1, h, z2), s); va->AddVertexQC(float3(x1, 0.f, z2), e);
		va->AddVertexQC(float3(x2, h, z2), s); va->AddVertexQC(float3(x2, 0.f, z2), e);
		va->AddVertexQC(float3(x2, h, z1), s); va->AddVertexQC(float3(x2, 0.f, z1), e);
		va->DrawArrayC(GL_LINES);

		va = GetVertexArray();
		va->Initialize();
		va->AddVertexQC(float3(x1, 0.0f, z1), e);
		va->AddVertexQC(float3(x1, 0.0f, z2), e);
		va->AddVertexQC(float3(x2, 0.0f, z2), e);
		va->AddVertexQC(float3(x2, 0.0f, z1), e);
		va->DrawArrayC(GL_LINE_LOOP);
	}

	glEnable(GL_DEPTH_TEST);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	// glDisable(GL_BLEND);

	return canBuild;
}



inline const icon::CIconData* GetUnitIcon(const CUnit* unit) {
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];
	const unsigned short prevMask = (LOS_PREVLOS | LOS_CONTRADAR);

	const UnitDef* unitDef = unit->unitDef;
	const icon::CIconData* iconData = nullptr;

	// use the unit's custom icon if we can currently see it,
	// or have seen it before and did not lose contact since
	bool unitVisible = ((losStatus & (LOS_INLOS | LOS_INRADAR)) && ((losStatus & prevMask) == prevMask));
	unitVisible |= gameSetup->ghostedBuildings && unit->unitDef->IsBuildingUnit() && (losStatus & LOS_PREVLOS);
	const bool customIcon = (minimap->UseUnitIcons() && (unitVisible || gu->spectatingFullView));

	if (customIcon)
		return (unitDef->iconType.GetIconData());

	if ((losStatus & LOS_INRADAR) != 0)
		iconData = icon::iconHandler.GetDefaultIconData();

	return iconData;
}

inline float GetUnitIconScale(const CUnit* unit) {
	float scale = unit->myIcon->GetSize();

	if (!minimap->UseUnitIcons())
		return scale;
	if (!unit->myIcon->GetRadiusAdjust())
		return scale;

	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];
	const unsigned short prevMask = (LOS_PREVLOS | LOS_CONTRADAR);
	const bool unitVisible = ((losStatus & LOS_INLOS) || ((losStatus & LOS_INRADAR) && ((losStatus & prevMask) == prevMask)));

	if ((unitVisible || gu->spectatingFullView)) {
		scale *= (unit->radius / unit->myIcon->GetRadiusScale());
	}

	return scale;
}


void CUnitDrawer::DrawUnitMiniMapIcon(const CUnit* unit, CVertexArray* va) const {
	if (unit->noMinimap)
		return;
	if (unit->myIcon == nullptr)
		return;
	if (unit->IsInVoid())
		return;

	const unsigned char defaultColor[4] = {255, 255, 255, 255};
	const unsigned char* color = &defaultColor[0];

	if (!unit->isSelected) {
		if (minimap->UseSimpleColors()) {
			if (unit->team == gu->myTeam) {
				color = minimap->GetMyTeamIconColor();
			} else if (teamHandler.Ally(gu->myAllyTeam, unit->allyteam)) {
				color = minimap->GetAllyTeamIconColor();
			} else {
				color = minimap->GetEnemyTeamIconColor();
			}
		} else {
			color = teamHandler.Team(unit->team)->color;
		}
	}

	const float iconScale = GetUnitIconScale(unit);
	const float3& iconPos = (!gu->spectatingFullView) ?
		unit->GetObjDrawErrorPos(gu->myAllyTeam):
		unit->GetObjDrawMidPos();

	const float iconSizeX = (iconScale * minimap->GetUnitSizeX());
	const float iconSizeY = (iconScale * minimap->GetUnitSizeY());

	const float x0 = iconPos.x - iconSizeX;
	const float x1 = iconPos.x + iconSizeX;
	const float y0 = iconPos.z - iconSizeY;
	const float y1 = iconPos.z + iconSizeY;

	unit->myIcon->DrawArray(va, x0, y0, x1, y1, color);
}

void CUnitDrawer::DrawUnitMiniMapIcons() const {
	CVertexArray* va = GetVertexArray();

	for (auto iconIt = unitsByIcon.cbegin(); iconIt != unitsByIcon.cend(); ++iconIt) {
		const icon::CIconData* icon = iconIt->first;
		const std::vector<const CUnit*>& units = iconIt->second;

		if (icon == nullptr)
			continue;
		if (units.empty())
			continue;

		va->Initialize();
		va->EnlargeArrays(units.size() * 4, 0, VA_SIZE_2DTC);
		icon->BindTexture();

		for (const CUnit* unit: units) {
			assert(unit->myIcon == icon);
			DrawUnitMiniMapIcon(unit, va);
		}

		va->DrawArray2dTC(GL_QUADS);
	}
}


void CUnitDrawer::UpdateUnitDefMiniMapIcons(const UnitDef* ud)
{
	for (int teamNum = 0; teamNum < teamHandler.ActiveTeams(); teamNum++) {
		for (const CUnit* unit: unitHandler.GetUnitsByTeamAndDef(teamNum, ud->id)) {
			UpdateUnitMiniMapIcon(unit, true, false);
		}
	}
}

void CUnitDrawer::UpdateUnitMiniMapIcon(const CUnit* unit, bool forced, bool killed) {
	CUnit* u = const_cast<CUnit*>(unit);

	icon::CIconData* oldIcon = unit->myIcon;
	icon::CIconData* newIcon = const_cast<icon::CIconData*>(GetUnitIcon(unit));

	u->myIcon = nullptr;

	if (!killed) {
		if ((oldIcon != newIcon) || forced) {
			spring::VectorErase(unitsByIcon[oldIcon], unit);
			unitsByIcon[newIcon].push_back(unit);
		}

		u->myIcon = newIcon;
		return;
	}

	spring::VectorErase(unitsByIcon[oldIcon], unit);
}



void CUnitDrawer::RenderUnitCreated(const CUnit* u, int cloaked) {
	CUnit* unit = const_cast<CUnit*>(u);

	if (u->model != nullptr) {
		if (cloaked) {
			alphaModelRenderers[MDL_TYPE(u)].AddObject(u);
		} else {
			opaqueModelRenderers[MDL_TYPE(u)].AddObject(u);
		}
	}

	UpdateUnitMiniMapIcon(u, false, false);
	unsortedUnits.push_back(unit);
}

void CUnitDrawer::RenderUnitDestroyed(const CUnit* unit) {
	CUnit* u = const_cast<CUnit*>(unit);

	const UnitDef* unitDef = unit->unitDef;
	const UnitDef* decoyDef = unitDef->decoyDef;

	const bool addNewGhost = unitDef->IsBuildingUnit() && gameSetup->ghostedBuildings;

	// TODO - make ghosted buildings per allyTeam - so they are correctly dealt with
	// when spectating
	GhostSolidObject* gso = nullptr;
	// FIXME -- adjust decals for decoys? gets weird?
	S3DModel* gsoModel = (decoyDef == nullptr)? u->model: decoyDef->LoadModel();

	for (int allyTeam = 0; allyTeam < deadGhostBuildings.size(); ++allyTeam) {
		const bool canSeeGhost = !(u->losStatus[allyTeam] & (LOS_INLOS | LOS_CONTRADAR)) && (u->losStatus[allyTeam] & (LOS_PREVLOS));

		if (addNewGhost && canSeeGhost) {
			if (gso == nullptr) {
				gso = ghostMemPool.alloc<GhostSolidObject>();
				gso->pos    = u->pos;
				gso->model  = gsoModel;
				gso->decal  = nullptr;
				gso->facing = u->buildFacing;
				gso->dir    = u->frontdir;
				gso->team   = u->team;
				gso->refCount = 0;
				gso->lastDrawFrame = 0;

				groundDecals->GhostCreated(u, gso);
			}

			// <gso> can be inserted for multiple allyteams
			// (the ref-counter saves us come deletion time)
			deadGhostBuildings[allyTeam][gsoModel->type].push_back(gso);
			gso->IncRef();
		}

		spring::VectorErase(liveGhostBuildings[allyTeam][MDL_TYPE(u)], u);
	}

	if (u->model != nullptr) {
		// delete from both; cloaked state is unreliable at this point
		alphaModelRenderers[MDL_TYPE(u)].DelObject(u);
		opaqueModelRenderers[MDL_TYPE(u)].DelObject(u);
	}

	spring::VectorErase(unsortedUnits, u);

	UpdateUnitMiniMapIcon(unit, false, true);
	LuaObjectDrawer::SetObjectLOD(u, LUAOBJ_UNIT, 0);
}


void CUnitDrawer::UnitCloaked(const CUnit* unit) {
	CUnit* u = const_cast<CUnit*>(unit);

	if (u->model != nullptr) {
		alphaModelRenderers[MDL_TYPE(u)].AddObject(u);
		opaqueModelRenderers[MDL_TYPE(u)].DelObject(u);
	}
}

void CUnitDrawer::UnitDecloaked(const CUnit* unit) {
	CUnit* u = const_cast<CUnit*>(unit);

	if (u->model != nullptr) {
		opaqueModelRenderers[MDL_TYPE(u)].AddObject(u);
		alphaModelRenderers[MDL_TYPE(u)].DelObject(u);
	}
}

void CUnitDrawer::UnitEnteredLos(const CUnit* unit, int allyTeam) {
	CUnit* u = const_cast<CUnit*>(unit); //cleanup

	if (gameSetup->ghostedBuildings && unit->unitDef->IsBuildingUnit())
		spring::VectorErase(liveGhostBuildings[allyTeam][MDL_TYPE(unit)], u);

	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}

void CUnitDrawer::UnitLeftLos(const CUnit* unit, int allyTeam) {
	CUnit* u = const_cast<CUnit*>(unit); //cleanup

	if (gameSetup->ghostedBuildings && unit->unitDef->IsBuildingUnit())
		spring::VectorInsertUnique(liveGhostBuildings[allyTeam][MDL_TYPE(unit)], u, true);

	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}

void CUnitDrawer::UnitEnteredRadar(const CUnit* unit, int allyTeam) {
	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}

void CUnitDrawer::UnitLeftRadar(const CUnit* unit, int allyTeam) {
	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}


void CUnitDrawer::PlayerChanged(int playerNum) {
	if (playerNum != gu->myPlayerNum)
		return;

	for (auto iconIt = unitsByIcon.begin(); iconIt != unitsByIcon.end(); ++iconIt) {
		(iconIt->second).clear();
	}

	for (CUnit* unit: unsortedUnits) {
		// force an erase (no-op) followed by an insert
		UpdateUnitMiniMapIcon(unit, true, false);
	}
}

void CUnitDrawer::SunChanged() {
	unitDrawerStates[DRAWER_STATE_SEL]->UpdateCurrentShaderSky(this, sky->GetLight());
}



bool CUnitDrawer::ObjectVisibleReflection(const float3 objPos, const float3 camPos, float maxRadius)
{
	if (objPos.y < 0.0f)
		return (CGround::GetApproximateHeight(objPos.x, objPos.z, false) <= maxRadius);

	const float dif = objPos.y - camPos.y;

	float3 zeroPos;
	zeroPos += (camPos * ( objPos.y / dif));
	zeroPos += (objPos * (-camPos.y / dif));

	return (CGround::GetApproximateHeight(zeroPos.x, zeroPos.z, false) <= maxRadius);
}



void CUnitDrawer::AddTempDrawUnit(const TempDrawUnit& tdu)
{
	const UnitDef* unitDef = tdu.unitDef;
	const S3DModel* model = unitDef->LoadModel();

	if (tdu.drawAlpha) {
		tempAlphaUnits[model->type].push_back(tdu);
	} else {
		tempOpaqueUnits[model->type].push_back(tdu);
	}
}

void CUnitDrawer::UpdateTempDrawUnits(std::vector<TempDrawUnit>& tempDrawUnits)
{
	for (unsigned int n = 0; n < tempDrawUnits.size(); /*no-op*/) {
		if (tempDrawUnits[n].timeout <= gs->frameNum) {
			// do not use spring::VectorErase; we already know the index
			tempDrawUnits[n] = tempDrawUnits.back();
			tempDrawUnits.pop_back();
			continue;
		}

		n += 1;
	}
}






static bool LoadBuildPic(const std::string& filename, CBitmap& bitmap)
{
	if (CFileHandler::FileExists(filename, SPRING_VFS_RAW_FIRST)) {
		bitmap.Load(filename);
		return true;
	}

	return false;
}

void CUnitDrawer::SetUnitDefImage(const UnitDef* unitDef, const std::string& texName)
{
	UnitDefImage*& unitImage = unitDef->buildPic;

	if (unitImage == nullptr) {
		unitImage = &unitDefImages[unitDef->id];
	} else {
		unitImage->Free();
	}

	CBitmap bitmap;

	if (!texName.empty()) {
		bitmap.Load("unitpics/" + texName);
	} else {
		if (!LoadBuildPic("unitpics/" + unitDef->name + ".dds", bitmap) &&
		    !LoadBuildPic("unitpics/" + unitDef->name + ".png", bitmap) &&
		    !LoadBuildPic("unitpics/" + unitDef->name + ".pcx", bitmap) &&
		    !LoadBuildPic("unitpics/" + unitDef->name + ".bmp", bitmap)) {
			bitmap.AllocDummy(SColor(255, 0, 0, 255));
		}
	}

	unitImage->textureID = bitmap.CreateTexture();
	unitImage->imageSizeX = bitmap.xsize;
	unitImage->imageSizeY = bitmap.ysize;
}

void CUnitDrawer::SetUnitDefImage(const UnitDef* unitDef, unsigned int texID, int xsize, int ysize)
{
	UnitDefImage*& unitImage = unitDef->buildPic;

	if (unitImage == nullptr) {
		unitImage = &unitDefImages[unitDef->id];
	} else {
		unitImage->Free();
	}

	unitImage->textureID = texID;
	unitImage->imageSizeX = xsize;
	unitImage->imageSizeY = ysize;
}

unsigned int CUnitDrawer::GetUnitDefImage(const UnitDef* unitDef)
{
	if (unitDef->buildPic == nullptr)
		SetUnitDefImage(unitDef, unitDef->buildPicName);

	return (unitDef->buildPic->textureID);
}

