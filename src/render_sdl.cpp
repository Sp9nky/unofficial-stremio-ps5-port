#include "render_sdl.h"

#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "artcache.h"
#include "util.h"

namespace {

struct Geometry {
	std::vector<SDL_Vertex> vertices;
	std::vector<int> indices;
};

inline SDL_Color unpremultiply(const Rml::ColourbPremultiplied& c) {
	SDL_Color out;
	out.a = c.alpha;
	if (c.alpha == 0) {
		out.r = out.g = out.b = 0;
	} else if (c.alpha == 255) {
		out.r = c.red, out.g = c.green, out.b = c.blue;
	} else {
		out.r = Uint8(std::min(255, c.red * 255 / c.alpha));
		out.g = Uint8(std::min(255, c.green * 255 / c.alpha));
		out.b = Uint8(std::min(255, c.blue * 255 / c.alpha));
	}
	return out;
}

SDL_Texture* make_texture(SDL_Renderer* r, const unsigned char* rgba, int w, int h) {
	SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
	if (!t) {
		dlog("SDL_CreateTexture %dx%d: %s", w, h, SDL_GetError());
		return nullptr;
	}
	SDL_UpdateTexture(t, nullptr, rgba, w * 4);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

}  // namespace

Rml::CompiledGeometryHandle RenderSDL::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                       Rml::Span<const int> indices) {
	auto* g = new Geometry;
	g->vertices.resize(vertices.size());
	for (size_t i = 0; i < vertices.size(); i++) {
		const Rml::Vertex& v = vertices[i];
		SDL_Vertex& o = g->vertices[i];
		o.position = {v.position.x, v.position.y};
		o.color = unpremultiply(v.colour);
		o.tex_coord = {v.tex_coord.x, v.tex_coord.y};
	}
	g->indices.assign(indices.begin(), indices.end());
	return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

static bool same_color(const SDL_Color& a, const SDL_Color& b) {
	return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Two triangles (6 indices) covering an axis-aligned rectangle with one
// colour: boxes, images and glyphs. Drawn as a rect fill or a texture copy.
bool RenderSDL::draw_rect(const SDL_Vertex* verts, const int* idx, SDL_Texture* tex) {
	int corner[4], n = 0;
	for (int i = 0; i < 6; i++) {
		bool seen = false;
		for (int j = 0; j < n; j++) seen |= corner[j] == idx[i];
		if (seen) continue;
		if (n == 4) return false;
		corner[n++] = idx[i];
	}
	if (n != 4) return false;
	float x0 = verts[corner[0]].position.x, x1 = x0, y0 = verts[corner[0]].position.y, y1 = y0;
	for (int i = 1; i < 4; i++) {
		const SDL_FPoint& p = verts[corner[i]].position;
		x0 = std::min(x0, p.x), x1 = std::max(x1, p.x);
		y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
	}
	if (!(x1 > x0 && y1 > y0)) return false;
	// Each vertex on a different corner, all one colour.
	const SDL_Vertex* at[4] = {nullptr, nullptr, nullptr, nullptr};  // TL, TR, BL, BR
	for (int i = 0; i < 4; i++) {
		const SDL_Vertex& v = verts[corner[i]];
		bool l = v.position.x == x0, r = v.position.x == x1, t = v.position.y == y0, b = v.position.y == y1;
		if (!(l || r) || !(t || b)) return false;
		int k = (r ? 1 : 0) + (b ? 2 : 0);
		if (at[k]) return false;
		at[k] = &v;
		if (!same_color(v.color, verts[corner[0]].color)) return false;
	}
	// The shared edge must be a diagonal, or the triangles overlap.
	int shared = 0;
	for (int i = 0; i < 3; i++)
		for (int j = 3; j < 6; j++)
			if (idx[i] == idx[j]) shared++;
	if (shared != 2) return false;
	for (int i = 0; i < 3; i++)
		for (int j = i + 1; j < 3; j++) {
			const SDL_FPoint &a = verts[idx[i]].position, &b = verts[idx[j]].position;
			bool in_second = false, in_second2 = false;
			for (int k = 3; k < 6; k++) in_second |= idx[k] == idx[i], in_second2 |= idx[k] == idx[j];
			if (in_second && in_second2 && (a.x == b.x || a.y == b.y)) return false;
		}

	const SDL_Color c = at[0]->color;
	SDL_FRect dst = {x0, y0, x1 - x0, y1 - y0};
	if (!tex) {
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
		SDL_RenderFillRectF(renderer_, &dst);
		return true;
	}
	const SDL_FPoint &tl = at[0]->tex_coord, &br = at[3]->tex_coord;
	if (at[1]->tex_coord.x != br.x || at[1]->tex_coord.y != tl.y || at[2]->tex_coord.x != tl.x ||
	    at[2]->tex_coord.y != br.y || !(br.x > tl.x) || !(br.y > tl.y))
		return false;
	int tw = 0, th = 0;
	SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
	SDL_Rect src;
	src.x = int(std::lround(tl.x * tw));
	src.y = int(std::lround(tl.y * th));
	src.w = int(std::lround(br.x * tw)) - src.x;
	src.h = int(std::lround(br.y * th)) - src.y;
	if (src.w <= 0 || src.h <= 0) return false;
	SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
	SDL_SetTextureAlphaMod(tex, c.a);
	SDL_RenderCopyF(renderer_, tex, &src, &dst);
	// Triangles drawn with this texture take their colour from the vertices.
	SDL_SetTextureColorMod(tex, 255, 255, 255);
	SDL_SetTextureAlphaMod(tex, 255);
	return true;
}

void RenderSDL::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
	auto* g = reinterpret_cast<Geometry*>(handle);
	if (!g || g->indices.empty()) return;
	static thread_local std::vector<SDL_Vertex> moved;
	const SDL_Vertex* verts = g->vertices.data();
	if (translation.x != 0 || translation.y != 0) {
		moved = g->vertices;
		for (auto& v : moved) {
			v.position.x += translation.x;
			v.position.y += translation.y;
		}
		verts = moved.data();
	}
	// SDL's software renderer merges neighbouring triangles into rectangles
	// even when they aren't (RmlUi's rounded boxes are triangle fans), which
	// draws parts of translucent boxes twice. So rectangles are drawn here and
	// any other triangle goes to SDL on its own, where nothing gets merged.
	auto* tex = reinterpret_cast<SDL_Texture*>(texture);
	const int* idx = g->indices.data();
	const size_t count = g->indices.size();
	const int nverts = int(g->vertices.size());
	for (size_t i = 0; i + 3 <= count;) {
		if (i + 6 <= count && draw_rect(verts, idx + i, tex)) {
			i += 6;
			continue;
		}
		SDL_RenderGeometry(renderer_, tex, verts, nverts, idx + i, 3);
		i += 3;
	}
}

void RenderSDL::ReleaseGeometry(Rml::CompiledGeometryHandle handle) { delete reinterpret_cast<Geometry*>(handle); }

Rml::TextureHandle RenderSDL::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
	std::vector<unsigned char> px;
	int w = 0, h = 0;
	if (!load_image_rgba(source, px, w, h)) {
		dlog("texture: can't load %s", source.c_str());
		return 0;
	}
	SDL_Texture* t = make_texture(renderer_, px.data(), w, h);
	if (!t) return 0;
	dimensions = {w, h};
	return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::TextureHandle RenderSDL::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
	// Font glyphs and similar, premultiplied: convert to straight alpha.
	std::vector<unsigned char> px(source.begin(), source.end());
	for (size_t i = 0; i + 3 < px.size(); i += 4) {
		unsigned a = px[i + 3];
		if (a == 0 || a == 255) continue;
		px[i + 0] = (unsigned char)std::min(255u, px[i + 0] * 255u / a);
		px[i + 1] = (unsigned char)std::min(255u, px[i + 1] * 255u / a);
		px[i + 2] = (unsigned char)std::min(255u, px[i + 2] * 255u / a);
	}
	return reinterpret_cast<Rml::TextureHandle>(make_texture(renderer_, px.data(), dimensions.x, dimensions.y));
}

void RenderSDL::ReleaseTexture(Rml::TextureHandle texture) {
	if (texture) SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(texture));
}

void RenderSDL::EnableScissorRegion(bool enable) {
	scissor_enabled_ = enable;
	SDL_RenderSetClipRect(renderer_, enable ? &scissor_ : nullptr);
}

void RenderSDL::SetScissorRegion(Rml::Rectanglei region) {
	scissor_ = {region.Left(), region.Top(), region.Width(), region.Height()};
	if (scissor_enabled_) SDL_RenderSetClipRect(renderer_, &scissor_);
}

void RenderSDL::BeginFrame() {
	scissor_enabled_ = false;
	SDL_RenderSetClipRect(renderer_, nullptr);
}

// ---------------------------------------------------------------------------

double SystemSDL::GetElapsedTime() {
	static const double start = now_seconds();
	return now_seconds() - start;
}

bool SystemSDL::LogMessage(Rml::Log::Type type, const Rml::String& message) {
	const char* kind = "info";
	switch (type) {
	case Rml::Log::LT_ERROR: kind = "error"; break;
	case Rml::Log::LT_ASSERT: kind = "assert"; break;
	case Rml::Log::LT_WARNING: kind = "warning"; break;
	case Rml::Log::LT_DEBUG: return true;
	default: break;
	}
	dlog("rmlui %s: %s", kind, message.c_str());
	return true;
}

void SystemSDL::JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) {
	if (!path.empty() && path[0] == '/') {
		translated_path = path;
		return;
	}
	Rml::SystemInterface::JoinPath(translated_path, document_path, path);
}
