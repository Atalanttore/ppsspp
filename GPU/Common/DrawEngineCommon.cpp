// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>

#include "Common/ColorConv.h"
#include "Core/Config.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"

#define QUAD_INDICES_MAX 65536

DrawEngineCommon::DrawEngineCommon()
	: dec_(nullptr),
		decOptions_{},
		fboTexNeedBind_(false),
		fboTexBound_(false) {
	quadIndices_ = new u16[6 * QUAD_INDICES_MAX];
	decJitCache_ = new VertexDecoderJitCache();
}

DrawEngineCommon::~DrawEngineCommon() {
	delete[] quadIndices_;
	delete decJitCache_;
	for (auto iter = decoderMap_.begin(); iter != decoderMap_.end(); iter++) {
		delete iter->second;
	}
}

VertexDecoder *DrawEngineCommon::GetVertexDecoder(u32 vtype) {
	auto iter = decoderMap_.find(vtype);
	if (iter != decoderMap_.end())
		return iter->second;
	VertexDecoder *dec = new VertexDecoder();
	dec->SetVertexType(vtype, decOptions_, decJitCache_);
	decoderMap_[vtype] = dec;
	return dec;
}

std::vector<std::string> DrawEngineCommon::DebugGetVertexLoaderIDs() {
	std::vector<std::string> ids;
	for (auto iter : decoderMap_) {
		std::string id;
		id.resize(sizeof(iter.first));
		memcpy(&id[0], &iter.first, sizeof(iter.first));
		ids.push_back(id);
	}
	return ids;
}

std::string DrawEngineCommon::DebugGetVertexLoaderString(std::string id, DebugShaderStringType stringType) {
	u32 mapId;
	memcpy(&mapId, &id[0], sizeof(mapId));
	auto iter = decoderMap_.find(mapId);
	if (iter == decoderMap_.end())
		return "N/A";
	else
		return iter->second->GetString(stringType);
}

struct Plane {
	float x, y, z, w;
	void Set(float _x, float _y, float _z, float _w) { x = _x; y = _y; z = _z; w = _w; }
	float Test(float f[3]) const { return x * f[0] + y * f[1] + z * f[2] + w; }
};

static void PlanesFromMatrix(float mtx[16], Plane planes[6]) {
	planes[0].Set(mtx[3]-mtx[0], mtx[7]-mtx[4], mtx[11]-mtx[8], mtx[15]-mtx[12]);  // Right
	planes[1].Set(mtx[3]+mtx[0], mtx[7]+mtx[4], mtx[11]+mtx[8], mtx[15]+mtx[12]);  // Left
	planes[2].Set(mtx[3]+mtx[1], mtx[7]+mtx[5], mtx[11]+mtx[9], mtx[15]+mtx[13]);  // Bottom
	planes[3].Set(mtx[3]-mtx[1], mtx[7]-mtx[5], mtx[11]-mtx[9], mtx[15]-mtx[13]);  // Top
	planes[4].Set(mtx[3]+mtx[2], mtx[7]+mtx[6], mtx[11]+mtx[10], mtx[15]+mtx[14]); // Near
	planes[5].Set(mtx[3]-mtx[2], mtx[7]-mtx[6], mtx[11]-mtx[10], mtx[15]-mtx[14]); // Far
}

static Vec3f ClipToScreen(const Vec4f& coords) {
	float xScale = gstate.getViewportXScale();
	float xCenter = gstate.getViewportXCenter();
	float yScale = gstate.getViewportYScale();
	float yCenter = gstate.getViewportYCenter();
	float zScale = gstate.getViewportZScale();
	float zCenter = gstate.getViewportZCenter();

	float x = coords.x * xScale / coords.w + xCenter;
	float y = coords.y * yScale / coords.w + yCenter;
	float z = coords.z * zScale / coords.w + zCenter;

	// 16 = 0xFFFF / 4095.9375
	return Vec3f(x * 16, y * 16, z);
}

static Vec3f ScreenToDrawing(const Vec3f& coords) {
	Vec3f ret;
	ret.x = (coords.x - gstate.getOffsetX16()) * (1.0f / 16.0f);
	ret.y = (coords.y - gstate.getOffsetY16()) * (1.0f / 16.0f);
	ret.z = coords.z;
	return ret;
}

u32 DrawEngineCommon::NormalizeVertices(u8 *outPtr, u8 *bufPtr, const u8 *inPtr, int lowerBound, int upperBound, u32 vertType) {
	const u32 vertTypeID = (vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24);
	VertexDecoder *dec = GetVertexDecoder(vertTypeID);
	return DrawEngineCommon::NormalizeVertices(outPtr, bufPtr, inPtr, dec, lowerBound, upperBound, vertType);
}

void DrawEngineCommon::ApplyClearToMemory(int x1, int y1, int x2, int y2, u32 clearColor) {
	u8 *addr = Memory::GetPointer(gstate.getFrameBufAddress());
	const bool singleByteClear = (clearColor >> 16) == (clearColor & 0xFFFF) && (clearColor >> 24) == (clearColor & 0xFF);
	const int bpp = gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2;
	const int stride = gstate.FrameBufStride();
	const int width = x2 - x1;

	// Can use memset for simple cases. Often alpha is different and gums up the works.
	// The check for bpp==4 etc is because we don't properly convert the clear color to the correct
	// 16-bit format before computing the singleByteClear value. That could be done, but it was easier
	// to just fall back to the generic case.
	if (singleByteClear && (bpp == 4 || clearColor == 0)) {
		const int byteStride = stride * bpp;
		const int byteWidth = width * bpp;
		addr += x1 * bpp;
		for (int y = y1; y < y2; ++y) {
			memset(addr + y * byteStride, clearColor, byteWidth);
		}
	} else {
		u16 clear16 = 0;
		switch (gstate.FrameBufFormat()) {
		case GE_FORMAT_565: ConvertRGBA8888ToRGB565(&clear16, &clearColor, 1); break;
		case GE_FORMAT_5551: ConvertRGBA8888ToRGBA5551(&clear16, &clearColor, 1); break;
		case GE_FORMAT_4444: ConvertRGBA8888ToRGBA4444(&clear16, &clearColor, 1); break;
		}

		// This will most often be true - rarely is the width not aligned.
		// TODO: We should really use non-temporal stores here to avoid the cache,
		// as it's unlikely that these bytes will be read.
		if ((width & 3) == 0 && (x1 & 3) == 0) {
			u64 val64 = clearColor | ((u64)clearColor << 32);
			int xstride = 2;
			if (bpp == 2) {
				// Spread to all eight bytes.
				u64 c2 = clear16 | (clear16 << 16);
				val64 = c2 | (c2 << 32);
				xstride = 4;
			}

			u64 *addr64 = (u64 *)addr;
			const int stride64 = stride / xstride;
			const int x1_64 = x1 / xstride;
			const int x2_64 = x2 / xstride;
			for (int y = y1; y < y2; ++y) {
				for (int x = x1_64; x < x2_64; ++x) {
					addr64[y * stride64 + x] = val64;
				}
			}
		} else if (bpp == 4) {
			u32 *addr32 = (u32 *)addr;
			for (int y = y1; y < y2; ++y) {
				for (int x = x1; x < x2; ++x) {
					addr32[y * stride + x] = clearColor;
				}
			}
		} else if (bpp == 2) {
			u16 *addr16 = (u16 *)addr;
			for (int y = y1; y < y2; ++y) {
				for (int x = x1; x < x2; ++x) {
					addr16[y * stride + x] = clear16;
				}
			}
		}
	}
}

// This code is HIGHLY unoptimized!
//
// It does the simplest and safest test possible: If all points of a bbox is outside a single of
// our clipping planes, we reject the box. Tighter bounds would be desirable but would take more calculations.
bool DrawEngineCommon::TestBoundingBox(void* control_points, int vertexCount, u32 vertType) {
	SimpleVertex *corners = (SimpleVertex *)(decoded + 65536 * 12);
	float *verts = (float *)(decoded + 65536 * 18);

	// Try to skip NormalizeVertices if it's pure positions. No need to bother with a vertex decoder
	// and a large vertex format.
	if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_FLOAT) {
		verts = (float *)control_points;
	} else if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_8BIT) {
		const s8 *vtx = (const s8 *)control_points;
		for (int i = 0; i < vertexCount * 3; i++) {
			verts[i] = vtx[i] * (1.0f / 128.0f);
		}
	} else if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_16BIT) {
		const s16 *vtx = (const s16*)control_points;
		for (int i = 0; i < vertexCount * 3; i++) {
			verts[i] = vtx[i] * (1.0f / 32768.0f);
		}
	} else {
		// Simplify away bones and morph before proceeding
		u8 *temp_buffer = decoded + 65536 * 24;
		NormalizeVertices((u8 *)corners, temp_buffer, (u8 *)control_points, 0, vertexCount, vertType);
		for (int i = 0; i < vertexCount; i++) {
			verts[i * 3] = corners[i].pos.x;
			verts[i * 3 + 1] = corners[i].pos.y;
			verts[i * 3 + 2] = corners[i].pos.z;
		}
	}

	Plane planes[6];

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);
	PlanesFromMatrix(worldviewproj, planes);
	for (int plane = 0; plane < 6; plane++) {
		int inside = 0;
		int out = 0;
		for (int i = 0; i < vertexCount; i++) {
			// Here we can test against the frustum planes!
			float value = planes[plane].Test(verts + i * 3);
			if (value < 0)
				out++;
			else
				inside++;
		}

		if (inside == 0) {
			// All out
			return false;
		}

		// Any out. For testing that the planes are in the right locations.
		// if (out != 0) return false;
	}

	return true;
}

// TODO: This probably is not the best interface.
bool DrawEngineCommon::GetCurrentSimpleVertices(int count, std::vector<GPUDebugVertex> &vertices, std::vector<u16> &indices) {
	// This is always for the current vertices.
	u16 indexLowerBound = 0;
	u16 indexUpperBound = count - 1;

	if (!Memory::IsValidAddress(gstate_c.vertexAddr))
		return false;

	bool savedVertexFullAlpha = gstate_c.vertexFullAlpha;

	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		const u8 *inds = Memory::GetPointer(gstate_c.indexAddr);
		const u16 *inds16 = (const u16 *)inds;
		const u32 *inds32 = (const u32 *)inds;

		if (inds) {
			GetIndexBounds(inds, count, gstate.vertType, &indexLowerBound, &indexUpperBound);
			indices.resize(count);
			switch (gstate.vertType & GE_VTYPE_IDX_MASK) {
			case GE_VTYPE_IDX_8BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds[i];
				}
				break;
			case GE_VTYPE_IDX_16BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds16[i];
				}
				break;
			case GE_VTYPE_IDX_32BIT:
				WARN_LOG_REPORT_ONCE(simpleIndexes32, G3D, "SimpleVertices: Decoding 32-bit indexes");
				for (int i = 0; i < count; ++i) {
					// These aren't documented and should be rare.  Let's bounds check each one.
					if (inds32[i] != (u16)inds32[i]) {
						ERROR_LOG_REPORT_ONCE(simpleIndexes32Bounds, G3D, "SimpleVertices: Index outside 16-bit range");
					}
					indices[i] = (u16)inds32[i];
				}
				break;
			}
		} else {
			indices.clear();
		}
	} else {
		indices.clear();
	}

	static std::vector<u32> temp_buffer;
	static std::vector<SimpleVertex> simpleVertices;
	temp_buffer.resize(std::max((int)indexUpperBound, 8192) * 128 / sizeof(u32));
	simpleVertices.resize(indexUpperBound + 1);
	NormalizeVertices((u8 *)(&simpleVertices[0]), (u8 *)(&temp_buffer[0]), Memory::GetPointer(gstate_c.vertexAddr), indexLowerBound, indexUpperBound, gstate.vertType);

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);

	vertices.resize(indexUpperBound + 1);
	for (int i = indexLowerBound; i <= indexUpperBound; ++i) {
		const SimpleVertex &vert = simpleVertices[i];

		if (gstate.isModeThrough()) {
			if (gstate.vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0];
				vertices[i].v = vert.uv[1];
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = vert.pos.x;
			vertices[i].y = vert.pos.y;
			vertices[i].z = vert.pos.z;
			if (gstate.vertType & GE_VTYPE_COL_MASK) {
				memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
			} else {
				memset(vertices[i].c, 0, sizeof(vertices[i].c));
			}
		} else {
			float clipPos[4];
			Vec3ByMatrix44(clipPos, vert.pos.AsArray(), worldviewproj);
			Vec3f screenPos = ClipToScreen(clipPos);
			Vec3f drawPos = ScreenToDrawing(screenPos);

			if (gstate.vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0] * (float)gstate.getTextureWidth(0);
				vertices[i].v = vert.uv[1] * (float)gstate.getTextureHeight(0);
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = drawPos.x;
			vertices[i].y = drawPos.y;
			vertices[i].z = drawPos.z;
			if (gstate.vertType & GE_VTYPE_COL_MASK) {
				memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
			} else {
				memset(vertices[i].c, 0, sizeof(vertices[i].c));
			}
		}
	}

	gstate_c.vertexFullAlpha = savedVertexFullAlpha;

	return true;
}


// This normalizes a set of vertices in any format to SimpleVertex format, by processing away morphing AND skinning.
// The rest of the transform pipeline like lighting will go as normal, either hardware or software.
// The implementation is initially a bit inefficient but shouldn't be a big deal.
// An intermediate buffer of not-easy-to-predict size is stored at bufPtr.
u32 DrawEngineCommon::NormalizeVertices(u8 *outPtr, u8 *bufPtr, const u8 *inPtr, VertexDecoder *dec, int lowerBound, int upperBound, u32 vertType) {
	// First, decode the vertices into a GPU compatible format. This step can be eliminated but will need a separate
	// implementation of the vertex decoder.
	dec->DecodeVerts(bufPtr, inPtr, lowerBound, upperBound);

	// OK, morphing eliminated but bones still remain to be taken care of.
	// Let's do a partial software transform where we only do skinning.

	VertexReader reader(bufPtr, dec->GetDecVtxFmt(), vertType);

	SimpleVertex *sverts = (SimpleVertex *)outPtr;

	const u8 defaultColor[4] = {
		(u8)gstate.getMaterialAmbientR(),
		(u8)gstate.getMaterialAmbientG(),
		(u8)gstate.getMaterialAmbientB(),
		(u8)gstate.getMaterialAmbientA(),
	};

	// Let's have two separate loops, one for non skinning and one for skinning.
	if (!g_Config.bSoftwareSkinning && (vertType & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE) {
		int numBoneWeights = vertTypeGetNumBoneWeights(vertType);
		for (int i = lowerBound; i <= upperBound; i++) {
			reader.Goto(i - lowerBound);
			SimpleVertex &sv = sverts[i];
			if (vertType & GE_VTYPE_TC_MASK) {
				reader.ReadUV(sv.uv);
			}

			if (vertType & GE_VTYPE_COL_MASK) {
				reader.ReadColor0_8888(sv.color);
			} else {
				memcpy(sv.color, defaultColor, 4);
			}

			float nrm[3], pos[3];
			float bnrm[3], bpos[3];

			if (vertType & GE_VTYPE_NRM_MASK) {
				// Normals are generated during tessellation anyway, not sure if any need to supply
				reader.ReadNrm(nrm);
			} else {
				nrm[0] = 0;
				nrm[1] = 0;
				nrm[2] = 1.0f;
			}
			reader.ReadPos(pos);

			// Apply skinning transform directly
			float weights[8];
			reader.ReadWeights(weights);
			// Skinning
			Vec3Packedf psum(0, 0, 0);
			Vec3Packedf nsum(0, 0, 0);
			for (int w = 0; w < numBoneWeights; w++) {
				if (weights[w] != 0.0f) {
					Vec3ByMatrix43(bpos, pos, gstate.boneMatrix + w * 12);
					Vec3Packedf tpos(bpos);
					psum += tpos * weights[w];

					Norm3ByMatrix43(bnrm, nrm, gstate.boneMatrix + w * 12);
					Vec3Packedf tnorm(bnrm);
					nsum += tnorm * weights[w];
				}
			}
			sv.pos = psum;
			sv.nrm = nsum;
		}
	} else {
		for (int i = lowerBound; i <= upperBound; i++) {
			reader.Goto(i - lowerBound);
			SimpleVertex &sv = sverts[i];
			if (vertType & GE_VTYPE_TC_MASK) {
				reader.ReadUV(sv.uv);
			} else {
				sv.uv[0] = 0.0f;  // This will get filled in during tessellation
				sv.uv[1] = 0.0f;
			}
			if (vertType & GE_VTYPE_COL_MASK) {
				reader.ReadColor0_8888(sv.color);
			} else {
				memcpy(sv.color, defaultColor, 4);
			}
			if (vertType & GE_VTYPE_NRM_MASK) {
				// Normals are generated during tessellation anyway, not sure if any need to supply
				reader.ReadNrm((float *)&sv.nrm);
			} else {
				sv.nrm.x = 0.0f;
				sv.nrm.y = 0.0f;
				sv.nrm.z = 1.0f;
			}
			reader.ReadPos((float *)&sv.pos);
		}
	}

	// Okay, there we are! Return the new type (but keep the index bits)
	return GE_VTYPE_TC_FLOAT | GE_VTYPE_COL_8888 | GE_VTYPE_NRM_FLOAT | GE_VTYPE_POS_FLOAT | (vertType & (GE_VTYPE_IDX_MASK | GE_VTYPE_THROUGH));
}

bool DrawEngineCommon::ApplyShaderBlending() {
	if (gstate_c.featureFlags & GPU_SUPPORTS_ANY_FRAMEBUFFER_FETCH) {
		return true;
	}

	static const int MAX_REASONABLE_BLITS_PER_FRAME = 24;

	static int lastFrameBlit = -1;
	static int blitsThisFrame = 0;
	if (lastFrameBlit != gpuStats.numFlips) {
		if (blitsThisFrame > MAX_REASONABLE_BLITS_PER_FRAME) {
			WARN_LOG_REPORT_ONCE(blendingBlit, G3D, "Lots of blits needed for obscure blending: %d per frame, blend %d/%d/%d", blitsThisFrame, gstate.getBlendFuncA(), gstate.getBlendFuncB(), gstate.getBlendEq());
		}
		blitsThisFrame = 0;
		lastFrameBlit = gpuStats.numFlips;
	}
	++blitsThisFrame;
	if (blitsThisFrame > MAX_REASONABLE_BLITS_PER_FRAME * 2) {
		WARN_LOG_ONCE(blendingBlit2, G3D, "Skipping additional blits needed for obscure blending: %d per frame, blend %d/%d/%d", blitsThisFrame, gstate.getBlendFuncA(), gstate.getBlendFuncB(), gstate.getBlendEq());
		return false;
	}

	fboTexNeedBind_ = true;

	gstate_c.Dirty(DIRTY_SHADERBLEND);
	return true;
}
