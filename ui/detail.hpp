// A title's page: its artwork behind, its details on the left, and on the
// right the episodes (series) or the streams to pick from.

#pragma once

#include "cards.hpp"
#include "content.hpp"
#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"

namespace sx
{

class Detail
{
  public:
    enum class Mode
    {
        episodes,
        streams,
    };

    explicit Detail(const Env &env) : env_(env)
    {
    }

    void set_content(const DetailContent *content);
    // The content changed in place (details, episodes or streams arrived).
    void refresh();
    int episode_index() const
    {
        return episode_;
    }
    int stream_index() const
    {
        return stream_;
    }
    void enter();
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &list, hui::gfx::BackdropSpec &backdrop) const;

    Mode mode() const
    {
        return mode_;
    }
    void set_mode(Mode mode);

  private:
    hui::gfx::Rect episode_rect(int index, bool final_position) const;
    hui::gfx::Rect stream_rect(int index, bool final_position) const;
    void draw_info(hui::gfx::DrawList &list) const;
    void draw_episodes(hui::gfx::DrawList &list) const;
    void draw_streams(hui::gfx::DrawList &list) const;
    void draw_hints_row(hui::gfx::DrawList &list) const;

    const Env &env_;
    const DetailContent *content_ = nullptr;
    Mode mode_ = Mode::streams;
    int streams_serial_ = 0;
    bool streams_wanted_ = false; // the user asked for an episode's streams
    int hint_serial_ = 0;
    int episode_ = 0;
    int stream_ = 0;
    hui::ui::Scroller episode_scroll_;
    hui::ui::Scroller stream_scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Timer swap_; // the panel's content changing
    float age_ = 0.0f;
    float clock_ = 0.0f;
};

} // namespace sx
