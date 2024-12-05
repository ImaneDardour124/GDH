#include "recorder.hpp"
#include "hacks.hpp"
#include "replayEngine.hpp"
#include <Geode/modify/ShaderLayer.hpp>
#include "gui.hpp"
#include <imgui-cocos.hpp>
#ifdef GEODE_IS_WINDOWS
#include <subprocess.hpp>
#endif

class $modify(ShaderLayer)
{
	void visit()
	{
        auto& recorder = Recorder::get();
		recorder.shader_visiting = true;
		ShaderLayer::visit();
		recorder.shader_visiting = false;
	}
};

intptr_t glViewportAddress = 0;
    
void glViewportHook(GLint a, GLint b, GLsizei c, GLsizei d) {
    auto& recorder = Recorder::get();
    if (recorder.is_recording && recorder.playlayer_visiting && recorder.shader_visiting) {
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        // geode::log::debug("upscaling shader? {}x{} = {}x{} ({})", c, d, static_cast<int>(displaySize.x), static_cast<int>(displaySize.y), (c == static_cast<int>(displaySize.x) && d == static_cast<int>(displaySize.y)));
        if (c != 2608 && d != 2608 && c != 1304 && d != 1304 && c != 652 && d != 652 && c == static_cast<int>(displaySize.x) && d == static_cast<int>(displaySize.y)) {
            c = recorder.width;
            d = recorder.height;
        }
    }

    reinterpret_cast<void(__stdcall *)(GLint, GLint, GLsizei, GLsizei)>(glViewportAddress)(a, b, c, d);
}

#ifdef GEODE_IS_WINDOWS
$execute {
    glViewportAddress = geode::addresser::getNonVirtual(glViewport);
    auto result = geode::Mod::get()->hook(reinterpret_cast<void *>(glViewportAddress), &glViewportHook, "glViewport");
}
#endif

void RenderTexture::begin() {   
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &oldFBO);

    texture = new cocos2d::CCTexture2D;
    {
        auto data = malloc(width * height * 3);
        memset(data, 0, width * height * 3);
        texture->initWithData(data, cocos2d::kCCTexture2DPixelFormat_RGB888, width, height, cocos2d::CCSize(static_cast<float>(width), static_cast<float>(height)));
        free(data);
    }

    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &oldRBO);

    glGenFramebuffersEXT(1, &currentFBO);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, currentFBO);

    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture->getName(), 0);
    
    texture->setAliasTexParameters();
    texture->autorelease();

    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, oldRBO);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, oldFBO);    
}

void RenderTexture::capture_frame(std::mutex& lock, std::vector<uint8_t>& data, volatile bool& frame_has_data) {
    auto& recorder = Recorder::get();
    glViewport(0, 0, width, height);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &oldFBO);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, currentFBO);

    auto director = cocos2d::CCDirector::sharedDirector();
    auto scene = GameManager::sharedState()->getPlayLayer();

    recorder.playlayer_visiting = true;
    scene->visit();
    recorder.playlayer_visiting = false;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    lock.lock();
    frame_has_data = true;
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data.data());
    lock.unlock();

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, oldFBO);
    director->setViewport();
}

void RenderTexture::end() {
    if (texture)
        texture->release();
}

void Recorder::start(std::string command) {
    #ifdef GEODE_IS_WINDOWS
    need_remove_black = false;
    need_visible_lc = false;

    last_frame_time = extra_time = 0;

    after_end_extra_time = 0.f;

    frame_has_data = false;
    current_frame.resize(width * height * 3, 0);
    is_recording = true;
    texture.height = height;
    texture.width = width;
    texture.begin();

    std::thread([&, command] {            
        auto process = subprocess::Popen(command);
        while (is_recording || frame_has_data) {
            lock.lock();
            if (frame_has_data) {
                const auto frame = current_frame;
                frame_has_data = false;
                process.m_stdin.write(frame.data(), frame.size());                
            }
            lock.unlock();
        }

        if (process.close()) {
            return;
        }
    }).detach();
    #endif
}

void Recorder::stop() {
    texture.end();

    is_recording = false;
    enabled = false;
    
    auto pl = PlayLayer::get();
    
    if (pl && fade_out) {
        need_remove_black = true;
    }

    if (pl && hide_level_complete) {
        need_visible_lc = true;
    }
    
}

void Recorder::render_frame() {
    while (frame_has_data) {}
    texture.capture_frame(lock, current_frame, frame_has_data);
}

void Recorder::handle_recording(float dt) {
    auto playLayer = PlayLayer::get();
    if (!playLayer->m_hasCompletedLevel || after_end_extra_time < after_end_duration) {
        if (playLayer->m_hasCompletedLevel) {
            after_end_extra_time += dt;
        }

        double frame_dt = 1.0 / static_cast<double>(fps);
        double time = (playLayer->m_gameState.m_levelTime - delay) + extra_time - last_frame_time;
        if (time >= frame_dt) {
            auto fmod = FMODAudioEngine::get();
            auto offset = (playLayer->m_levelSettings->m_songOffset + playLayer->m_gameState.m_levelTime) * 1000.0;
            fmod->setMusicTimeMS(offset, false, 0);

            extra_time = time - frame_dt;
            last_frame_time = (playLayer->m_gameState.m_levelTime - delay);
            render_frame();
        }
    }
    else {
        stop();
    }   
}

std::string Recorder::compile_command() {
    std::string command = fmt::format("ffmpeg.exe -y -f rawvideo -pix_fmt rgb24 -s {}x{} -r {} -i -", width, height, fps);

    if (!codec.empty()) {
        command += fmt::format(" -c:v {}", codec);
    }

    if (!bitrate.empty()) {
        command += fmt::format(" -b:v {}", bitrate);
    }

    if (!extra_args.empty()) {
        command += fmt::format(" {}", extra_args);
    }
    else {
        command += " -pix_fmt yuv420p";
    }
    
    if (!vf_args.empty())
        command += fmt::format(" -vf {}", vf_args);

    command += fmt::format(" -an \"{}\\{}\"", folderShowcasesPath, video_name);

    return command;
}

void Recorder::compile_vf_args() {
    vf_args = "";
    if (vflip) {
        vf_args += "\"vflip\"";
    }

    if (fade_in) {
        if (vflip) {
            vf_args += ",";
        }

        vf_args += fmt::format("\"fade=t=in:st={}:d={}\"", fade_in_start, fade_in_end);
    }
}

void RecorderAudio::start() {
    is_recording = true;
    after_end_extra_time = 0;

    auto fmod_engine = FMODAudioEngine::get();
    
    old_volume_music = fmod_engine->getBackgroundMusicVolume();
    old_volume_sfx = fmod_engine->getEffectsVolume();

    fmod_engine->setBackgroundMusicVolume(1.f);
    fmod_engine->setEffectsVolume(1.f);

    fmod_engine->m_system->setOutput(FMOD_OUTPUTTYPE_WAVWRITER);
}

void RecorderAudio::stop() {
    enabled = false;
    is_recording = false;

    auto fmod_engine = FMODAudioEngine::get();
    fmod_engine->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);

    fmod_engine->setBackgroundMusicVolume(old_volume_music);
    fmod_engine->setEffectsVolume(old_volume_sfx);

    // imgui_popup::add_popup("Audio recording stoped!");

    if (std::filesystem::exists("fmodoutput.wav")) {
        try {
            std::filesystem::rename("fmodoutput.wav", Recorder::get().folderShowcasesPath / audio_name);
        }
        catch (const std::filesystem::filesystem_error& e) {
            geode::log::error("Error moving file: {}", e.what());
        }
    }    
}

void RecorderAudio::handle_recording(float dt) {
    auto playLayer = GameManager::sharedState()->getPlayLayer();
    if (showcase_mode) {
        if (!playLayer->m_hasCompletedLevel || after_end_extra_time < after_end_duration) {
            if (playLayer->m_hasCompletedLevel) {
                after_end_extra_time += dt;
            }
        }
        else {
            stop();
        } 
    }  
}