#include "BgfxEngine.h"

#include <Runtime/RuntimeImpl.h>
#include <Engine/NapiBridge.h>
#include <Engine/ShaderCompiler.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

// TODO: this needs to be fixed in bgfx
namespace bgfx
{
    uint16_t attribToId(Attrib::Enum _attr);
}

#define BGFX_UNIFORM_FRAGMENTBIT UINT8_C(0x10) // Copy-pasta from bgfx_p.h
#define BGFX_UNIFORM_SAMPLERBIT  UINT8_C(0x20) // Copy-pasta from bgfx_p.h

#include <bimg/bimg.h>
#include <bimg/decode.h>
#include <bimg/encode.h>

#include <bx/math.h>
#include <bx/readerwriter.h>

#include <regex>
#include <sstream>

namespace babylon
{
    namespace
    {
        struct UniformData final
        {
            uint8_t Stage{};
            bgfx::UniformHandle Uniform{};
        };

        template<typename AppendageT>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const AppendageT appendage)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(&appendage);
            auto stride = static_cast<std::ptrdiff_t>(sizeof(AppendageT));
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        template<typename AppendageT = std::string&>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const std::string& string)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(string.data());
            auto stride = static_cast<std::ptrdiff_t>(string.length());
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        template<typename ElementT>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const gsl::span<ElementT>& data)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(data.data());
            auto stride = static_cast<std::ptrdiff_t>(data.size() * sizeof(ElementT));
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        void FlipYInImageBytes(gsl::span<uint8_t> bytes, size_t rowCount, size_t rowPitch)
        {
            std::vector<uint8_t> buffer{};
            buffer.reserve(rowPitch);

            for (size_t row = 0; row < rowCount / 2; row++)
            {
                auto frontPtr = bytes.data() + (row * rowPitch);
                auto backPtr = bytes.data() + ((rowCount - row - 1) * rowPitch);

                std::memcpy(buffer.data(), frontPtr, rowPitch);
                std::memcpy(frontPtr, backPtr, rowPitch);
                std::memcpy(backPtr, buffer.data(), rowPitch);
            }
        }

        void AppendUniformBuffer(std::vector<uint8_t>& bytes, const spirv_cross::Compiler& compiler, const spirv_cross::Resource& uniformBuffer, bool isFragment)
        {
            const uint8_t fragmentBit = (isFragment ? BGFX_UNIFORM_FRAGMENTBIT : 0);

            const spirv_cross::SPIRType& type = compiler.get_type(uniformBuffer.base_type_id);
            for (uint32_t index = 0; index < type.member_types.size(); ++index)
            {
                auto name = compiler.get_member_name(uniformBuffer.base_type_id, index);
                auto offset = compiler.get_member_decoration(uniformBuffer.base_type_id, index, spv::DecorationOffset);
                auto memberType = compiler.get_type(type.member_types[index]);

                bgfx::UniformType::Enum bgfxType;
                uint16_t regCount;

                if (memberType.basetype != spirv_cross::SPIRType::Float)
                {
                    throw std::exception("Not supported");
                }

                if (!memberType.array.empty())
                {
                    throw std::exception("Not implemented");
                }

                if (memberType.columns == 1 && 1 <= memberType.vecsize && memberType.vecsize <= 4)
                {
                    bgfxType = bgfx::UniformType::Vec4;
                    regCount = 1;
                }
                else if (memberType.columns == 4 && memberType.vecsize == 4)
                {
                    bgfxType = bgfx::UniformType::Mat4;
                    regCount = 4;
                }
                else
                {
                    throw std::exception("Not supported");
                }

                AppendBytes(bytes, static_cast<uint8_t>(name.size()));
                AppendBytes(bytes, name);
                AppendBytes(bytes, static_cast<uint8_t>(bgfxType | fragmentBit));
                AppendBytes(bytes, static_cast<uint8_t>(0)); // Value "num" not used by D3D11 pipeline.
                AppendBytes(bytes, static_cast<uint16_t>(offset));
                AppendBytes(bytes, static_cast<uint16_t>(regCount));
            }
        }

        void AppendSamplers(std::vector<uint8_t>& bytes, const spirv_cross::Compiler& compiler, const spirv_cross::SmallVector<spirv_cross::Resource>& samplers, bool isFragment, std::map<const std::string, UniformData>& cache)
        {
            const uint8_t fragmentBit = (isFragment ? BGFX_UNIFORM_FRAGMENTBIT : 0);

            for (const spirv_cross::Resource& sampler : samplers)
            {
                AppendBytes(bytes, static_cast<uint8_t>(sampler.name.size()));
                AppendBytes(bytes, sampler.name);
                AppendBytes(bytes, static_cast<uint8_t>(bgfx::UniformType::Sampler | BGFX_UNIFORM_SAMPLERBIT));

                // These values (num, regIndex, regCount) are not used by D3D11 pipeline.
                AppendBytes(bytes, static_cast<uint8_t>(0));
                AppendBytes(bytes, static_cast<uint16_t>(0));
                AppendBytes(bytes, static_cast<uint16_t>(0));

                cache[sampler.name].Stage = compiler.get_decoration(sampler.id, spv::DecorationBinding);
            }
        }

        void CacheUniformHandles(bgfx::ShaderHandle shader, std::map<const std::string, UniformData>& cache)
        {
            const auto MAX_UNIFORMS = 256;
            bgfx::UniformHandle uniforms[MAX_UNIFORMS];
            auto numUniforms = bgfx::getShaderUniforms(shader, uniforms, MAX_UNIFORMS);

            bgfx::UniformInfo info{};
            for (uint8_t idx = 0; idx < numUniforms; idx++)
            {
                bgfx::getUniformInfo(uniforms[idx], info);
                cache[info.name].Uniform = uniforms[idx];
            }
        }

        enum class WebGLAttribType
        {
            BYTE = 5120,
            UNSIGNED_BYTE = 5121,
            SHORT = 5122,
            UNSIGNED_SHORT = 5123,
            INT = 5124,
            UNSIGNED_INT = 5125,
            FLOAT = 5126
        };

        bgfx::AttribType::Enum ConvertAttribType(WebGLAttribType type)
        {
            switch (type)
            {
            case WebGLAttribType::UNSIGNED_BYTE:    return bgfx::AttribType::Uint8;
            case WebGLAttribType::SHORT:            return bgfx::AttribType::Int16;
            case WebGLAttribType::FLOAT:            return bgfx::AttribType::Float;
            }

            throw std::exception("Unsupported attribute type");
        }
    }

    class BgfxEngine::Impl final
    {
    public:
        Impl(void* nativeWindowPtr, RuntimeImpl& runtimeImpl);
        ~Impl() = default;

        void Initialize(Napi::Env& env);
        void UpdateSize(float width, float height);
        void UpdateRenderTarget();
        void Suspend();

    private:
        using EngineDefiner = NativeEngineDefiner<BgfxEngine::Impl>;
        friend EngineDefiner;

        /*
            The sequence to draw the red box.

            CreateTexture()
            LoadTexture()
            GetTextureWidth()
            GetTextureHeight()
            GetTextureSampling()
            CreateIndexBuffer()
            RequestAnimationFrame()
            CreateProgram()
            GetUniforms()
            GetAttributes()
            SetProgram()
            Clear()
            GetRenderWidth()
            GetRenderHeight()
            GetRenderWidth()
            GetRenderHeight()
            SetState()
            CreateVertexArray()
            RecordIndexBuffer()
            CreateVertexBuffer()
            RecordVertexBuffer()
            RecordVertexBuffer()
            BindVertexArray()
            SetMatrix()
            SetMatrix()
            SetFloat4()
            SetFloat3()
            SetFloat4()
            SetFloat4()
            SetTextureWrapMode()
            SetTextureAnistrophicLevel()
            SetTexture()
            SetFloat4()
            SetFloat3()
            SetFloat4()
            SetFloat3()
            SetFloat4()
            DrawIndexed()
            RequestAnimationFrame()
            Present()
        */

        struct VertexArray final
        {
            bgfx::IndexBufferHandle indexBuffer;
            std::vector<bgfx::VertexBufferHandle> vertexBuffers;
        };

        enum BlendMode {}; // TODO DEBUG
        enum class Filter {}; // TODO DEBUG
        enum class AddressMode {}; // TODO DEBUG

        struct TextureData final
        {
            ~TextureData()
            {
                bgfx::destroy(Texture);

                for (auto image : Images)
                {
                    bimg::imageFree(image);
                }
            }

            std::vector<bimg::ImageContainer*> Images{};
            bgfx::TextureHandle Texture{};
        };

        struct ProgramData final
        {
            ~ProgramData()
            {
                bgfx::destroy(Program);
            }

            std::map<const std::string, uint32_t> AttributeLocations{};
            std::map<const std::string, UniformData> VertexUniformNameToHandle{};
            std::map<const std::string, UniformData> FragmentUniformNameToHandle{};

            bgfx::ProgramHandle Program{};
        };

        void RequestAnimationFrame(const Napi::CallbackInfo& info);
        Napi::Value CreateVertexArray(const Napi::CallbackInfo& info);
        void DeleteVertexArray(const Napi::CallbackInfo& info);
        void BindVertexArray(const Napi::CallbackInfo& info);
        Napi::Value CreateIndexBuffer(const Napi::CallbackInfo& info);
        void DeleteIndexBuffer(const Napi::CallbackInfo& info);
        void RecordIndexBuffer(const Napi::CallbackInfo& info);
        Napi::Value CreateVertexBuffer(const Napi::CallbackInfo& info);
        void DeleteVertexBuffer(const Napi::CallbackInfo& info);
        void RecordVertexBuffer(const Napi::CallbackInfo& info);
        Napi::Value CreateProgram(const Napi::CallbackInfo& info);
        Napi::Value GetUniforms(const Napi::CallbackInfo& info);
        Napi::Value GetAttributes(const Napi::CallbackInfo& info);
        void SetProgram(const Napi::CallbackInfo& info);
        void SetState(const Napi::CallbackInfo& info);
        void SetZOffset(const Napi::CallbackInfo& info);
        Napi::Value GetZOffset(const Napi::CallbackInfo& info);
        void SetDepthTest(const Napi::CallbackInfo& info);
        Napi::Value GetDepthWrite(const Napi::CallbackInfo& info);
        void SetDepthWrite(const Napi::CallbackInfo& info);
        void SetColorWrite(const Napi::CallbackInfo& info);
        void SetBlendMode(const Napi::CallbackInfo& info);
        void SetMatrix(const Napi::CallbackInfo& info);
        void SetIntArray(const Napi::CallbackInfo& info);
        void SetIntArray2(const Napi::CallbackInfo& info);
        void SetIntArray3(const Napi::CallbackInfo& info);
        void SetIntArray4(const Napi::CallbackInfo& info);
        void SetFloatArray(const Napi::CallbackInfo& info);
        void SetFloatArray2(const Napi::CallbackInfo& info);
        void SetFloatArray3(const Napi::CallbackInfo& info);
        void SetFloatArray4(const Napi::CallbackInfo& info);
        void SetMatrices(const Napi::CallbackInfo& info);
        void SetMatrix3x3(const Napi::CallbackInfo& info);
        void SetMatrix2x2(const Napi::CallbackInfo& info);
        void SetFloat(const Napi::CallbackInfo& info);
        void SetFloat2(const Napi::CallbackInfo& info);
        void SetFloat3(const Napi::CallbackInfo& info);
        void SetFloat4(const Napi::CallbackInfo& info);
        void SetBool(const Napi::CallbackInfo& info);
        Napi::Value CreateTexture(const Napi::CallbackInfo& info);
        void LoadTexture(const Napi::CallbackInfo& info);
        void LoadCubeTexture(const Napi::CallbackInfo& info);
        Napi::Value GetTextureWidth(const Napi::CallbackInfo& info);
        Napi::Value GetTextureHeight(const Napi::CallbackInfo& info);
        void SetTextureSampling(const Napi::CallbackInfo& info);
        void SetTextureWrapMode(const Napi::CallbackInfo& info);
        void SetTextureAnisotropicLevel(const Napi::CallbackInfo& info);
        void SetTexture(const Napi::CallbackInfo& info);
        void DeleteTexture(const Napi::CallbackInfo& info);
        void DrawIndexed(const Napi::CallbackInfo& info);
        void Draw(const Napi::CallbackInfo& info);
        void Clear(const Napi::CallbackInfo& info);
        Napi::Value GetRenderWidth(const Napi::CallbackInfo& info);
        Napi::Value GetRenderHeight(const Napi::CallbackInfo& info);

        void DispatchAnimationFrameAsync(Napi::FunctionReference callback);

        ShaderCompiler m_shaderCompiler;

        ProgramData* m_currentProgram;

        RuntimeImpl& m_runtimeImpl;

        struct
        {
            uint32_t Width{};
            uint32_t Height{};
        } m_size;

        bx::DefaultAllocator m_allocator;
        uint64_t m_engineState;
    };

    BgfxEngine::Impl::Impl(void* nativeWindowPtr, RuntimeImpl& runtimeImpl)
        : m_runtimeImpl{ runtimeImpl }
        , m_size{ 1024, 768 }
        , m_engineState{ BGFX_STATE_DEFAULT }
    {
        bgfx::Init init{};
        init.platformData.nwh = nativeWindowPtr;
        bgfx::setPlatformData(init.platformData);

        init.type = bgfx::RendererType::Direct3D11;
        init.resolution.width = m_size.Width;
        init.resolution.height = m_size.Height;
        init.resolution.reset = BGFX_RESET_VSYNC;
        bgfx::init(init);

        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, /*0xBB464BFF*/0x443355FF, 1.0f, 0);
        bgfx::setViewRect(0, 0, 0, m_size.Width, m_size.Height);

        // STUB: Let's get ready to RENNNDERRRRRRRRRRRR!!!
    }

    void BgfxEngine::Impl::Initialize(Napi::Env& env)
    {
        EngineDefiner::Define(env, this);
    }

    void BgfxEngine::Impl::UpdateSize(float width, float height)
    {
        auto w = static_cast<uint32_t>(width);
        auto h = static_cast<uint32_t>(height);

        if (w != m_size.Width || h != m_size.Height)
        {
            m_size = { w, h };
            UpdateRenderTarget();
        }
    }

    void BgfxEngine::Impl::UpdateRenderTarget()
    {
        bgfx::reset(m_size.Width, m_size.Height, BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4);
        bgfx::setViewRect(0, 0, 0, m_size.Width, m_size.Height);
    }

    void BgfxEngine::Impl::Suspend()
    {
        // TODO: Figure out what this is supposed to do.
    }

    // NativeEngine definitions

    void BgfxEngine::Impl::RequestAnimationFrame(const Napi::CallbackInfo& info)
    {
        DispatchAnimationFrameAsync(Napi::Persistent(info[0].As<Napi::Function>()));
    }

    Napi::Value BgfxEngine::Impl::CreateVertexArray(const Napi::CallbackInfo& info)
    {
        return Napi::External<VertexArray>::New(info.Env(), new VertexArray{});
    }

    void BgfxEngine::Impl::DeleteVertexArray(const Napi::CallbackInfo& info)
    {
        delete info[0].As<Napi::External<VertexArray>>().Data();
    }

    void BgfxEngine::Impl::BindVertexArray(const Napi::CallbackInfo& info)
    {
        const auto& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());

        bgfx::setIndexBuffer(vertexArray.indexBuffer);

        const auto numVertexBuffers = vertexArray.vertexBuffers.size();
        for (size_t index = 0; index < numVertexBuffers; ++index)
        {
            const auto& vertexBuffer = vertexArray.vertexBuffers[index];
            bgfx::setVertexBuffer(static_cast<uint8_t>(index), vertexBuffer);
        }
    }

    Napi::Value BgfxEngine::Impl::CreateIndexBuffer(const Napi::CallbackInfo& info)
    {
        const Napi::TypedArray data = info[0].As<Napi::TypedArray>();
        const bgfx::Memory* ref = bgfx::makeRef(data.As<Napi::Uint8Array>().Data(), static_cast<uint32_t>(data.ByteLength()));
        const uint16_t flags = data.TypedArrayType() == napi_typedarray_type::napi_uint16_array ? 0 : BGFX_BUFFER_INDEX32;
        const bgfx::IndexBufferHandle handle = bgfx::createIndexBuffer(ref, flags);
        return Napi::Value::From(info.Env(), static_cast<uint32_t>(handle.idx));
    }

    void BgfxEngine::Impl::DeleteIndexBuffer(const Napi::CallbackInfo& info)
    {
        const bgfx::IndexBufferHandle handle{ static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) };
        bgfx::destroy(handle);
    }

    void BgfxEngine::Impl::RecordIndexBuffer(const Napi::CallbackInfo& info)
    {
        VertexArray& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());
        const bgfx::IndexBufferHandle handle{ static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value()) };
        vertexArray.indexBuffer = handle;
    }

    Napi::Value BgfxEngine::Impl::CreateVertexBuffer(const Napi::CallbackInfo& info)
    {
        const Napi::Uint8Array data = info[0].As<Napi::Uint8Array>();
        const uint32_t byteStride = info[1].As<Napi::Number>().Uint32Value();
        const Napi::Array infos = info[2].As<Napi::Array>();

        bgfx::VertexDecl decl;
        decl.begin();
        const uint32_t infosLength = infos.Length();
        for (uint32_t index = 0; index < infosLength; index++)
        {
            const Napi::Object info = infos[index].As<Napi::Object>();
            const uint32_t location = info["location"].As<Napi::Number>().Uint32Value();
            const uint32_t numElements = info["numElements"].As<Napi::Number>().Uint32Value();
            const uint32_t type = info["type"].As<Napi::Number>().Uint32Value();
            const bool normalized = info["normalized"].As<Napi::Boolean>().Value();
            const uint32_t byteOffset = info["byteOffset"].As<Napi::Number>().Uint32Value();

            const bgfx::Attrib::Enum attrib = static_cast<bgfx::Attrib::Enum>(location);
            const bgfx::AttribType::Enum attribType = ConvertAttribType(static_cast<WebGLAttribType>(type));
            decl.add(attrib, numElements, attribType, normalized);
            decl.m_offset[attrib] = static_cast<uint16_t>(byteOffset);
        }
        decl.m_stride = static_cast<uint16_t>(byteStride);
        decl.end();

        const bgfx::Memory* ref = bgfx::copy(data.Data(), static_cast<uint32_t>(data.ByteLength()));
        const bgfx::VertexBufferHandle handle = bgfx::createVertexBuffer(ref, decl);
        return Napi::Value::From(info.Env(), static_cast<uint32_t>(handle.idx));
    }

    void BgfxEngine::Impl::DeleteVertexBuffer(const Napi::CallbackInfo& info)
    {
        const bgfx::VertexBufferHandle handle{ static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) };
        bgfx::destroy(handle);
    }

    void BgfxEngine::Impl::RecordVertexBuffer(const Napi::CallbackInfo& info)
    {
        VertexArray& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());
        const bgfx::VertexBufferHandle handle{ static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value()) };
        vertexArray.vertexBuffers.push_back(handle);
    }

    Napi::Value BgfxEngine::Impl::CreateProgram(const Napi::CallbackInfo& info)
    {
        auto vertexSource = info[0].As<Napi::String>().Utf8Value();
        // TODO: This is a HACK to account for the fact that DirectX and OpenGL disagree about the vertical orientation of screen space.
        // Remove this ASAP when we have a more long-term plan to account for this behavior.
        const auto fragmentSource = std::regex_replace(info[1].As<Napi::String>().Utf8Value(), std::regex("dFdy\\("), "-dFdy(");

        auto programData = new ProgramData();

        std::vector<uint8_t> vertexBytes{};
        std::vector<uint8_t> fragmentBytes{};
        std::map<const std::string, uint32_t> attributeLocations;

        m_shaderCompiler.Compile(vertexSource, fragmentSource, [&](ShaderCompiler::ShaderInfo vertexShaderInfo, ShaderCompiler::ShaderInfo fragmentShaderInfo)
        {
            constexpr uint8_t BGFX_SHADER_BIN_VERSION = 6;

            // These hashes are generated internally by BGFX's custom shader compilation pipeline,
            // which we don't have access to.  Fortunately, however, they aren't used for anything
            // crucial; they just have to match.
            constexpr uint32_t vertexOutputsHash = 0xBAD1DEA;
            constexpr uint32_t fragmentInputsHash = vertexOutputsHash;

            {
                const spirv_cross::Compiler& compiler = *vertexShaderInfo.Compiler;
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                assert(resources.uniform_buffers.size() == 1);
                const spirv_cross::Resource& uniformBuffer = resources.uniform_buffers[0];
                const spirv_cross::SmallVector<spirv_cross::Resource>& samplers = resources.separate_samplers;
                size_t numUniforms = compiler.get_type(uniformBuffer.base_type_id).member_types.size() + samplers.size();

                AppendBytes(vertexBytes, BX_MAKEFOURCC('V', 'S', 'H', BGFX_SHADER_BIN_VERSION));
                AppendBytes(vertexBytes, vertexOutputsHash);
                AppendBytes(vertexBytes, fragmentInputsHash);

                AppendBytes(vertexBytes, static_cast<uint16_t>(numUniforms));
                AppendUniformBuffer(vertexBytes, compiler, uniformBuffer, false);
                AppendSamplers(vertexBytes, compiler, samplers, false, programData->VertexUniformNameToHandle);

                AppendBytes(vertexBytes, static_cast<uint32_t>(vertexShaderInfo.Bytes.size()));
                AppendBytes(vertexBytes, vertexShaderInfo.Bytes);
                AppendBytes(vertexBytes, static_cast<uint8_t>(0));

                AppendBytes(vertexBytes, static_cast<uint8_t>(resources.stage_inputs.size()));
                for (const spirv_cross::Resource& stageInput : resources.stage_inputs)
                {
                    const uint32_t location = compiler.get_decoration(stageInput.id, spv::DecorationLocation);
                    AppendBytes(vertexBytes, bgfx::attribToId(static_cast<bgfx::Attrib::Enum>(location)));
                    attributeLocations[stageInput.name] = location;
                }

                AppendBytes(vertexBytes, static_cast<uint16_t>(compiler.get_declared_struct_size(compiler.get_type(uniformBuffer.base_type_id))));
            }

            {
                const spirv_cross::Compiler& compiler = *fragmentShaderInfo.Compiler;
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                assert(resources.uniform_buffers.size() == 1);
                const spirv_cross::Resource& uniformBuffer = resources.uniform_buffers[0];
                const spirv_cross::SmallVector<spirv_cross::Resource>& samplers = resources.separate_samplers;
                size_t numUniforms = compiler.get_type(uniformBuffer.base_type_id).member_types.size() + samplers.size();

                AppendBytes(fragmentBytes, BX_MAKEFOURCC('F', 'S', 'H', BGFX_SHADER_BIN_VERSION));
                AppendBytes(fragmentBytes, vertexOutputsHash);
                AppendBytes(fragmentBytes, fragmentInputsHash);

                AppendBytes(fragmentBytes, static_cast<uint16_t>(numUniforms));
                AppendUniformBuffer(fragmentBytes, compiler, uniformBuffer, true);
                AppendSamplers(fragmentBytes, compiler, samplers, true, programData->FragmentUniformNameToHandle);

                AppendBytes(fragmentBytes, static_cast<uint32_t>(fragmentShaderInfo.Bytes.size()));
                AppendBytes(fragmentBytes, fragmentShaderInfo.Bytes);
                AppendBytes(fragmentBytes, static_cast<uint8_t>(0));

                // Fragment shaders don't have attributes.
                AppendBytes(fragmentBytes, static_cast<uint8_t>(0));

                AppendBytes(fragmentBytes, static_cast<uint16_t>(compiler.get_declared_struct_size(compiler.get_type(uniformBuffer.base_type_id))));
            }
        });

        auto vertexShader = bgfx::createShader(bgfx::copy(vertexBytes.data(), static_cast<uint32_t>(vertexBytes.size())));
        CacheUniformHandles(vertexShader, programData->VertexUniformNameToHandle);
        programData->AttributeLocations = std::move(attributeLocations);

        auto fragmentShader = bgfx::createShader(bgfx::copy(fragmentBytes.data(), static_cast<uint32_t>(fragmentBytes.size())));
        CacheUniformHandles(fragmentShader, programData->FragmentUniformNameToHandle);

        programData->Program = bgfx::createProgram(vertexShader, fragmentShader);

        auto finalizer = [](Napi::Env, ProgramData* data)
        {
            delete data;
        };

        return Napi::External<ProgramData>::New(info.Env(), programData, finalizer);
    }

    Napi::Value BgfxEngine::Impl::GetUniforms(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        const auto names = info[1].As<Napi::Array>();

        auto length = names.Length();
        auto uniforms = Napi::Array::New(info.Env(), length);
        for (uint32_t index = 0; index < length; ++index)
        {
            const auto name = names[index].As<Napi::String>().Utf8Value();

            auto vertexFound = program->VertexUniformNameToHandle.find(name);
            auto fragmentFound = program->FragmentUniformNameToHandle.find(name);

            if (vertexFound != program->VertexUniformNameToHandle.end())
            {
                uniforms[index] = Napi::External<UniformData>::New(info.Env(), &vertexFound->second);
            }
            else if (fragmentFound != program->FragmentUniformNameToHandle.end())
            {
                uniforms[index] = Napi::External<UniformData>::New(info.Env(), &fragmentFound->second);
            }
            else
            {
                uniforms[index] = info.Env().Null();
            }
        }

        return uniforms;
    }

    Napi::Value BgfxEngine::Impl::GetAttributes(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        const auto names = info[1].As<Napi::Array>();

        const auto& attributeLocations = program->AttributeLocations;

        auto length = names.Length();
        auto attributes = Napi::Array::New(info.Env(), length);
        for (uint32_t index = 0; index < length; ++index)
        {
            const auto name = names[index].As<Napi::String>().Utf8Value();
            const auto it = attributeLocations.find(name);
            int location = (it == attributeLocations.end() ? -1 : gsl::narrow_cast<int>(it->second));
            attributes[index] = Napi::Value::From(info.Env(), location);
        }

        return attributes;
    }

    void BgfxEngine::Impl::SetProgram(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        m_currentProgram = program;
    }

    void BgfxEngine::Impl::SetState(const Napi::CallbackInfo& info)
    {
        const auto culling = info[0].As<Napi::Boolean>().Value();
        const auto reverseSide = info[2].As<Napi::Boolean>().Value();

        m_engineState &= ~BGFX_STATE_CULL_MASK;
        if (reverseSide)
        {
            m_engineState &= ~BGFX_STATE_FRONT_CCW;

            if (culling)
            {
                m_engineState |= BGFX_STATE_CULL_CW;
            }
        }
        else
        {
            m_engineState |= BGFX_STATE_FRONT_CCW;

            if (culling)
            {
                m_engineState |= BGFX_STATE_CULL_CCW;
            }
        }

        // TODO: zOffset
        const auto zOffset = info[1].As<Napi::Number>().FloatValue();

        bgfx::setState(m_engineState);
    }

    void BgfxEngine::Impl::SetZOffset(const Napi::CallbackInfo& info)
    {
        const auto zOffset = info[0].As<Napi::Number>().FloatValue();

        // STUB: Stub.
    }

    Napi::Value BgfxEngine::Impl::GetZOffset(const Napi::CallbackInfo& info)
    {
        // STUB: Stub.
        return{};
    }

    void BgfxEngine::Impl::SetDepthTest(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    Napi::Value BgfxEngine::Impl::GetDepthWrite(const Napi::CallbackInfo& info)
    {
        // STUB: Stub.
        return{};
    }

    void BgfxEngine::Impl::SetDepthWrite(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    void BgfxEngine::Impl::SetColorWrite(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    void BgfxEngine::Impl::SetBlendMode(const Napi::CallbackInfo& info)
    {
        const auto blendMode = static_cast<BlendMode>(info[0].As<Napi::Number>().Int32Value());

        m_engineState &= ~BGFX_STATE_BLEND_MASK;
        switch (blendMode)
        {
        case 0:
            break;
        case 25:
            m_engineState |= BGFX_STATE_BLEND_NORMAL;
            break;
        default:
            throw std::exception("Unsupported blend mode");
        }

        bgfx::setState(m_engineState);
    }

    void BgfxEngine::Impl::SetMatrix(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformData>>().Data();
        const auto matrix = info[1].As<Napi::Float32Array>();
        assert(matrix.ElementLength() == 16);

        bgfx::setUniform(uniformData->Uniform, matrix.Data(), 1);
    }

    void BgfxEngine::Impl::SetIntArray(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void BgfxEngine::Impl::SetIntArray2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void BgfxEngine::Impl::SetIntArray3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void BgfxEngine::Impl::SetIntArray4(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void BgfxEngine::Impl::SetFloatArray(const Napi::CallbackInfo& info)
    {
        const auto slot = info[0].As<Napi::Number>().Uint32Value();
        const auto array = info[1].As<Napi::Float32Array>();

        bgfx::UniformHandle handle{};
        // TODO: Check if the desired uniform already exists.  If so, use that.  If not, create one.

        bgfx::setUniform(handle, array.Data(), static_cast<uint16_t>(array.ElementLength())); // TODO: Padding?
    }

    void BgfxEngine::Impl::SetFloatArray2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void BgfxEngine::Impl::SetFloatArray3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void BgfxEngine::Impl::SetFloatArray4(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void BgfxEngine::Impl::SetMatrices(const Napi::CallbackInfo& info)
    {
        const auto slot = info[0].As<Napi::Number>().Uint32Value();
        const auto matricesArray = info[1].As<Napi::Float32Array>();
        assert(matricesArray.ElementLength() % 16 == 0);

        // STUB: Stub.
    }

    void BgfxEngine::Impl::SetMatrix3x3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> matrix

        assert(false);
    }

    void BgfxEngine::Impl::SetMatrix2x2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> matrix

        assert(false);
    }

    void BgfxEngine::Impl::SetFloat(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformData>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            0.0f,
            0.0f,
            0.0f
        };

        bgfx::setUniform(uniformData->Uniform, values, 1);
    }

    void BgfxEngine::Impl::SetFloat2(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformData>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            0.0f,
            0.0f
        };

        bgfx::setUniform(uniformData->Uniform, values, 1);
    }

    void BgfxEngine::Impl::SetFloat3(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformData>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue(),
            0.0f
        };

        bgfx::setUniform(uniformData->Uniform, values, 1);
    }

    void BgfxEngine::Impl::SetFloat4(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformData>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue(),
            info[4].As<Napi::Number>().FloatValue()
        };

        bgfx::setUniform(uniformData->Uniform, values, 1);
    }

    void BgfxEngine::Impl::SetBool(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, bool value

        assert(false);
    }

    Napi::Value BgfxEngine::Impl::CreateTexture(const Napi::CallbackInfo& info)
    {
        auto textureData = new TextureData();

        auto finalizer = [](Napi::Env, TextureData* data)
        {
            delete data;
        };

        return Napi::External<TextureData>::New(info.Env(), textureData, finalizer);
    }

    void BgfxEngine::Impl::LoadTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto buffer = info[1].As<Napi::ArrayBuffer>();
        const auto mipMap = info[2].As<Napi::Boolean>().Value();

        textureData->Images.push_back(bimg::imageParse(&m_allocator, buffer.Data(), static_cast<uint32_t>(buffer.ByteLength())));
        auto& image = *textureData->Images.front();

        bgfx::TextureFormat::Enum format{};
        switch (image.m_format)
        {
            case bimg::TextureFormat::RGBA8:
            {
                format = bgfx::TextureFormat::RGBA8;
                break;
            }
            case bimg::TextureFormat::RGB8:
            {
                format = bgfx::TextureFormat::RGB8;
                break;
            }
            default:
            {
                throw std::exception("Unexpected texture format.");
            }
        }

        textureData->Texture = bgfx::createTexture2D(
            image.m_width,
            image.m_height,
            false, // TODO: generate mipmaps when requested
            1,
            format,
            0,
            bgfx::makeRef(image.m_data, image.m_size));
    }

    void BgfxEngine::Impl::LoadCubeTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto mipLevelsArray = info[1].As<Napi::Array>();
        const auto flipY = info[2].As<Napi::Boolean>().Value();

        std::vector<std::vector<bimg::ImageContainer*>> images{};
        images.reserve(mipLevelsArray.Length());

        uint32_t totalSize = 0;

        for (uint32_t mipLevel = 0; mipLevel < mipLevelsArray.Length(); mipLevel++)
        {
            const auto facesArray = mipLevelsArray[mipLevel].As<Napi::Array>();

            images.emplace_back().reserve(facesArray.Length());

            for (uint32_t face = 0; face < facesArray.Length(); face++)
            {
                const auto image = facesArray[face].As<Napi::TypedArray>();
                auto buffer = gsl::make_span(static_cast<uint8_t*>(image.ArrayBuffer().Data()) + image.ByteOffset(), image.ByteLength());

                textureData->Images.push_back(bimg::imageParse(&m_allocator, buffer.data(), static_cast<uint32_t>(buffer.size())));
                images.back().push_back(textureData->Images.back());
                totalSize += static_cast<uint32_t>(images.back().back()->m_size);
            }
        }

        auto allPixels = bgfx::alloc(totalSize);

        auto ptr = allPixels->data;
        for (uint32_t face = 0; face < images.front().size(); face++)
        {
            for (uint32_t mipLevel = 0; mipLevel < images.size(); mipLevel++)
            {
                const auto image = images[mipLevel][face];

                std::memcpy(ptr, image->m_data, image->m_size);

                if (flipY)
                {
                    FlipYInImageBytes(gsl::make_span(ptr, image->m_size), image->m_height, image->m_size / image->m_height);
                }

                ptr += image->m_size;
            }
        }

        bgfx::TextureFormat::Enum format{};
        switch (images.front().front()->m_format)
        {
            case bimg::TextureFormat::RGBA8:
            {
                format = bgfx::TextureFormat::RGBA8;
                break;
            }
            case bimg::TextureFormat::RGB8:
            {
                format = bgfx::TextureFormat::RGB8;
                break;
            }
            default:
            {
                throw std::exception("Unexpected texture format.");
            }
        }

        textureData->Texture = bgfx::createTextureCube(
            images.front().front()->m_width,         // Side size
            true,                                           // Has mips
            1,                                              // Number of layers
            format,                                         // Self-explanatory
            0x0,                                            // Flags
            allPixels);                                     // Memory
    }

    Napi::Value BgfxEngine::Impl::GetTextureWidth(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        assert(textureData->Images.size() > 0 && !textureData->Images.front()->m_cubeMap);
        return Napi::Value::From(info.Env(), textureData->Images.front()->m_width);
    }

    Napi::Value BgfxEngine::Impl::GetTextureHeight(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        assert(textureData->Images.size() > 0 && !textureData->Images.front()->m_cubeMap);
        return Napi::Value::From(info.Env(), textureData->Images.front()->m_width);
    }

    void BgfxEngine::Impl::SetTextureSampling(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto filter = static_cast<Filter>(info[1].As<Napi::Number>().Uint32Value());

        // STUB: Stub.
    }

    void BgfxEngine::Impl::SetTextureWrapMode(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto addressModeU = static_cast<AddressMode>(info[1].As<Napi::Number>().Uint32Value());
        const auto addressModeV = static_cast<AddressMode>(info[2].As<Napi::Number>().Uint32Value());
        const auto addressModeW = static_cast<AddressMode>(info[3].As<Napi::Number>().Uint32Value());

        // STUB: Stub.
    }

    void BgfxEngine::Impl::SetTextureAnisotropicLevel(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto value = info[1].As<Napi::Number>().Uint32Value();

        // STUB: Stub.
    }

    void BgfxEngine::Impl::SetTexture(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformData>>().Data();
        const auto textureData = info[1].As<Napi::External<TextureData>>().Data();

        bgfx::setTexture(uniformData->Stage, uniformData->Uniform, textureData->Texture);
    }

    void BgfxEngine::Impl::DeleteTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        delete textureData;
    }

    void BgfxEngine::Impl::DrawIndexed(const Napi::CallbackInfo& info)
    {
        const auto fillMode = info[0].As<Napi::Number>().Int32Value();
        const auto elementStart = info[1].As<Napi::Number>().Int32Value();
        const auto elementCount = info[2].As<Napi::Number>().Int32Value();

        // TODO: handle viewport
        bgfx::submit(0, m_currentProgram->Program);
    }

    void BgfxEngine::Impl::Draw(const Napi::CallbackInfo& info)
    {
        const auto fillMode = info[0].As<Napi::Number>().Int32Value();
        const auto elementStart = info[1].As<Napi::Number>().Int32Value();
        const auto elementCount = info[2].As<Napi::Number>().Int32Value();

        // STUB: Stub.
        // bgfx::submit(), right?  Which means we have to preserve here the state of
        // which program is being worked on.
    }

    void BgfxEngine::Impl::Clear(const Napi::CallbackInfo& info)
    {
        auto r = info[0].As<Napi::Number>().FloatValue();
        auto g = info[1].As<Napi::Number>().FloatValue();
        auto b = info[2].As<Napi::Number>().FloatValue();
        auto a = info[3].As<Napi::Number>().FloatValue();
        auto backBuffer = info[4].As<Napi::Boolean>().Value();
        auto depth = info[5].As<Napi::Boolean>().Value();
        auto stencil = info[6].As<Napi::Boolean>().Value();

        // TODO CHECK: Does this have meaning for BGFX?  BGFX seems to call clear()
        // on its own, depending on the settings.
    }

    Napi::Value BgfxEngine::Impl::GetRenderWidth(const Napi::CallbackInfo& info)
    {
        // TODO CHECK: Is this not just the size?  What is this?
        return Napi::Value::From(info.Env(), m_size.Width);
    }

    Napi::Value BgfxEngine::Impl::GetRenderHeight(const Napi::CallbackInfo& info)
    {
        // TODO CHECK: Is this not just the size?  What is this?
        return Napi::Value::From(info.Env(), m_size.Height);
    }

    void BgfxEngine::Impl::DispatchAnimationFrameAsync(Napi::FunctionReference callback)
    {
        // The purpose of encapsulating the callbackPtr in a std::shared_ptr is because, under the hood, the lambda is
        // put into a kind of function which requires a copy constructor for all of its captured variables.  Because
        // the Napi::FunctionReference is not copyable, this breaks when trying to capture the callback directly, so we
        // wrap it in a std::shared_ptr to allow the capture to function correctly.
        m_runtimeImpl.Execute([this, callbackPtr = std::make_shared<Napi::FunctionReference>(std::move(callback))](auto&)
        {
            //bgfx_test(static_cast<uint16_t>(m_size.Width), static_cast<uint16_t>(m_size.Height));

            callbackPtr->Call({});
            bgfx::frame();
        });
    }

    // BgfxEngine exterior definitions.

    BgfxEngine::BgfxEngine(void* nativeWindowPtr, RuntimeImpl& runtimeImpl)
        : m_impl{ std::make_unique<BgfxEngine::Impl>(nativeWindowPtr, runtimeImpl) }
    {}

    BgfxEngine::~BgfxEngine()
    {}

    void BgfxEngine::Initialize(Napi::Env& env)
    {
        m_impl->Initialize(env);
    }

    void BgfxEngine::UpdateSize(float width, float height)
    {
        m_impl->UpdateSize(width, height);
    }

    void BgfxEngine::UpdateRenderTarget()
    {
        m_impl->UpdateRenderTarget();
    }

    void BgfxEngine::Suspend()
    {
        m_impl->Suspend();
    }
}