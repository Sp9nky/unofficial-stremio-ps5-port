#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL.h>

#include <string>

// RmlUi on SDL_Renderer (the PS5 SDL port renders in software).
//
// RmlUi 6 hands over premultiplied colours and textures, but the software
// renderer only has the straight-alpha SDL_BLENDMODE_BLEND, so everything is
// converted back to straight alpha on the way in.
class RenderSDL : public Rml::RenderInterface {
public:
	explicit RenderSDL(SDL_Renderer* renderer) : renderer_(renderer) {}

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
	                                            Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
	                    Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture) override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;

	void BeginFrame();

private:
	bool draw_rect(const SDL_Vertex* verts, const int* idx, SDL_Texture* tex);

	SDL_Renderer* renderer_;
	bool scissor_enabled_ = false;
	SDL_Rect scissor_{0, 0, 0, 0};
};

class SystemSDL : public Rml::SystemInterface {
public:
	double GetElapsedTime() override;
	bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
	// Absolute paths (artwork in the cache) are used as they are.
	void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override;
};
