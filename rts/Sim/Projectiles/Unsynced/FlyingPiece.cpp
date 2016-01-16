/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FlyingPiece.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/UnitDrawer.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/Textures/3DOTextureHandler.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Rendering/Models/3DOParser.h"

//#pragma optimize("tree-vectorize O3")


static const float EXPLOSION_SPEED = 3.f;


/////////////////////////////////////////////////////////////////////
/// SHARED
///

void FlyingPiece::InitCommon(const float3 _pos, const float3 _speed, const float _radius, int _team, int _texture)
{
	pos   = _pos;
	speed = _speed;
	radius = _radius + 10.f;

	texture = _texture;
	team    = _team;
}


void FlyingPiece::DrawCommon(size_t* lastTeam, size_t* lastTex, CVertexArray* const va)
{
	if (team == *lastTeam && texture == *lastTex)
		return;

	va->DrawArrayTN(GL_TRIANGLES);
	va->Initialize();

	if (team != *lastTeam) {
		*lastTeam = team;
		unitDrawer->SetTeamColour(team);
	}

	if (texture != *lastTex) {
		*lastTex = texture;
		texturehandlerS3O->SetS3oTexture(texture);
	}
}



/////////////////////////////////////////////////////////////////////
/// OLD 3DO IMPLEMENTATION (SLOW)
///

S3DOFlyingPiece::S3DOFlyingPiece(const float3& pos, const float3& speed, int team, const S3DOPiece* _piece, const S3DOPrimitive* _chunk)
: piece(_piece)
, chunk(_chunk)
{
	rotAxis  = gu->RandVector().ANormalize();
	rotSpeed = gu->RandFloat() * 0.1f;
	rotAngle = 0.0f;

	const std::vector<S3DOVertex>& vertices = piece->vertices;
	const std::vector<int>&         indices = chunk->vertices;
	float3 maxDist;
	for (int i = 0; i < 4; i++) {
		maxDist = float3::max(float3::fabs(vertices[indices[i]].pos), maxDist);
	}
	InitCommon(pos, speed + gu->RandVector() * EXPLOSION_SPEED, maxDist.Length(), team, -1);
}


unsigned S3DOFlyingPiece::GetTriangleCount() const
{
	return 2;
}


bool S3DOFlyingPiece::Update()
{
	pos      += speed;
	speed    *= 0.996f;
	speed.y  += mapInfo->map.gravity;
	rotAngle += rotSpeed;

	transMat.LoadIdentity();
	transMat.Rotate(rotAngle, rotAxis);

	return ((pos.y + radius) >= CGround::GetApproximateHeight(pos.x, pos.z, false));
}


void S3DOFlyingPiece::Draw(size_t* lastTeam, size_t* lastTex, CVertexArray* const va)
{
	DrawCommon(lastTeam, lastTex, va);
	va->EnlargeArrays(6, 0, VA_SIZE_TN);

	const float3 interPos = pos + speed * globalRendering->timeOffset;
	const C3DOTextureHandler::UnitTexture* tex = chunk->texture;
	const std::vector<S3DOVertex>& vertices = piece->vertices;
	const std::vector<int>&         indices = chunk->vertices;

	const float uvCoords[8] = {
		tex->xstart, tex->ystart,
		tex->xend,   tex->ystart,
		tex->xend,   tex->yend,
		tex->xstart, tex->yend
	};

	for (int i: {0,1,2, 0,2,3}) {
		const S3DOVertex& v = vertices[indices[i]];
		const float3 tp = transMat.Mul(v.pos) + interPos;
		const float3 tn = transMat.Mul(v.normal);
		va->AddVertexQTN(tp, uvCoords[i << 1], uvCoords[(i << 1) + 1], tn);
	}
}



/////////////////////////////////////////////////////////////////////
/// NEW S3O,OBJ,ASSIMP,... IMPLEMENTATION
///

SNewFlyingPiece::SNewFlyingPiece(
	const std::vector<SVertexData>& verts,
	const std::vector<unsigned int>& inds,
	const float3 pos,
	const float3 speed,
	const CMatrix44f& _pieceMatrix,
	const float2 _pieceParams, // (.x=radius, .y=chance)
	const int2 _renderParams // (.x=texType, .y=team)
)
: vertices(verts)
, indices(inds)
, pos0(pos)
, age(0)
, pieceRadius(_pieceParams.x)
, pieceMatrix(_pieceMatrix)
{
	InitCommon(pos, speed, pieceRadius, _renderParams.y, _renderParams.x);

	const size_t expectedSize = std::max<size_t>(1, _pieceParams.y * (indices.size() / 3));

	polygon.reserve(expectedSize);
	speeds.reserve(expectedSize);
	rotationAxisAndSpeed.reserve(expectedSize);

	for (size_t i = 0; i < indices.size(); i += 3) {
		if (gu->RandFloat() > _pieceParams.y)
			continue;

		polygon.push_back(i);
		speeds.push_back(speed + (GetPolygonDir(i) * EXPLOSION_SPEED * gu->RandFloat()));
		rotationAxisAndSpeed.emplace_back(gu->RandVector().ANormalize(), gu->RandFloat() * 0.1f);

		if (speeds.size() >= expectedSize)
			break;
	}
}


unsigned SNewFlyingPiece::GetTriangleCount() const
{
	return polygon.size();
}


bool SNewFlyingPiece::Update()
{
	++age;

	const float3 dragFactors = GetDragFactors();

	// used for `in camera frustum checks`
	pos    = pos0 + (speed * dragFactors.x) + UpVector * (mapInfo->map.gravity * dragFactors.y);
	radius = pieceRadius + EXPLOSION_SPEED * dragFactors.x + 10.f;

	// check visibility (if all particles are underground -> kill)
	if (age % GAME_SPEED != 0) {
		for (size_t i = 0; i < speeds.size(); ++i) {
			const CMatrix44f& m = GetMatrixOf(i, dragFactors);
			const float3 p = m.GetPos();
			if ((p.y + 10.f) >= CGround::GetApproximateHeight(p.x, p.z, false)) {
				return true;
			}
		}
		return false;
	}

	return true;
}


const SVertexData& SNewFlyingPiece::GetVertexData(unsigned short i) const
{
	assert((i < indices.size()) && (indices[i] < vertices.size()));
	return vertices[ indices[i] ];
}


float3 SNewFlyingPiece::GetPolygonDir(unsigned short idx) const
{
	float3 midPos;
	for (int j: {0,1,2}) {
		midPos += GetVertexData(idx + j).pos;
	}
	midPos *= 0.333f;
	return midPos.ANormalize();
}


float3 SNewFlyingPiece::GetDragFactors() const
{
	// We started with a naive (iterative) method like this:
	//  pos   += speed;
	//  speed *= airDrag;
	//  speed += gravity;
	// The problem is that pos & speed need to be saved for this (-> memory) and
	// need to be updated each frame (-> cpu usage). Doing so for each polygon is massively slow.
	// So I wanted to replace it with a stateless system, computing the current
	// position just from the params t & individual speed (start pos is 0!).
	//
	// To do so we split the computations in 2 parts: explosion speed & gravity.
	//
	// 1.
	// So the positional offset caused by the explosion speed becomes:
	//  d := drag   & s := explosion start speed
	//  xs(t=1) := s
	//  xs(t=2) := s + s * d
	//  xs(t=3) := s + s * d + s * d^2
	//  xs(t)   := s + s * d + s * d^2 + ... + s * d^t
	//           = s * sum(i=0,t){d^i}
	// That sum is a `geometric series`, the solution is `(1-d^(t+1))/(1-d)`.
	//  => xs(t) = s * (1 - d^(t+1)) / (1 - d)
	//           = s * speedDrag
	//
	// 2.
	// The positional offset caused by gravity is similar but a bit more complicated:
	//  xg(t=1) := g
	//  xg(t=2) := g + (g * d + g)                         = 2g + 1g*d
	//  xg(t=3) := g + (g * d + g) + (g * d^2 + g * d + g) = 3g + 2g*d + 1g*d^2
	//  xg(t)   := g * t + g * (t - 1) * d^2 + ... + g * (t - t) * d^t
	//           = g * sum(i=0,t){(t - i) * d^i}
	//           = g * (t * sum(i=0,t){d^i} - sum(i=0,t){i * d^i})
	//           = g * gravityDrag
	// The first sum is again a geometric series as above, the 2nd one is very similar.
	// The solution can be found here: https://de.wikipedia.org/w/index.php?title=Geometrische_Reihe&oldid=149159222#Verwandte_Summenformel_1
	//
	// The advantage of those 2 drag factors are that they only depend on the time (which even can be interpolated),
	// so we only have to compute them once per frame. And can then get the position of each particle with a simple multiplication,
	// saving memory & cpu time.

	static const float airDrag = 0.995f;
	static const float invAirDrag = 1.f / (1.f - airDrag);

	const float interAge = age + globalRendering->timeOffset;
	const float airDragPowOne = std::pow(airDrag, interAge + 1.f);
	const float airDragPowTwo = airDragPowOne * airDrag; // := std::pow(airDrag, interAge + 2.f);

	float3 dragFactors;

	// Speed Drag
	dragFactors.x = (1.f - airDragPowOne) * invAirDrag;

	// Gravity Drag
	dragFactors.y  = interAge * dragFactors.x; // 1st sum
	dragFactors.y -= (interAge * (airDragPowTwo - airDragPowOne) - airDragPowOne + airDrag) * invAirDrag * invAirDrag; // 2nd sum

	// time interpolation factor
	dragFactors.z = interAge;

	return dragFactors;
}


CMatrix44f SNewFlyingPiece::GetMatrixOf(const size_t i, const float3 dragFactors) const
{
	const float3 interPos = speeds[i] * dragFactors.x + UpVector * (mapInfo->map.gravity * dragFactors.y);
	const float4& rot = rotationAxisAndSpeed[i];

	CMatrix44f m = pieceMatrix;
	m.SetPos(m.GetPos() + interPos); //note: not the same as .Translate(pos) which does `m = m * translate(pos)`, but we want `m = translate(pos) * m`
	m.Rotate(rot.w * dragFactors.z, rot.xyz);

	return m;
}


void SNewFlyingPiece::Draw(size_t* lastTeam, size_t* lastTex, CVertexArray* const va)
{
	DrawCommon(lastTeam, lastTex, va);
	va->EnlargeArrays(speeds.size() * 3, 0, VA_SIZE_TN);

	const float3 dragFactors = GetDragFactors(); // speed, gravity

	for (size_t i = 0; i < speeds.size(); ++i) {
		const auto idx = polygon[i];
		const CMatrix44f& m = GetMatrixOf(i, dragFactors);

		for (int j: {0,1,2}) {
			const SVertexData& v = GetVertexData(idx + j);
			const float3 tp = m * v.pos;
			const float3 tn = m * float4(v.normal, 0.0f);
			va->AddVertexQTN(tp, v.texCoords[0].x, v.texCoords[0].y, tn); //FIXME use the model's VBOs & move matrix mult to shader
		}
	}
}

