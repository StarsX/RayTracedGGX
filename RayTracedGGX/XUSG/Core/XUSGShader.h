//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "XUSGType.h"

namespace XUSG
{
	namespace Shader
	{
		enum Stage : uint8_t
		{
			VS,
			PS,
			DS,
			HS,
			GS,
			CS,
			ALL = CS,

			NUM_GRAPHICS = ALL,
			NUM_STAGE,
		};
	}

	class ShaderPool
	{
	public:
		ShaderPool();
		virtual ~ShaderPool();

		void SetShader(Shader::Stage stage, uint32_t index, const Blob &shader);
		void SetShader(Shader::Stage stage, uint32_t index, const Blob &shader, const Shader::Reflector &reflector);
		void SetReflector(Shader::Stage stage, uint32_t index, const Shader::Reflector &reflector);

		Blob		CreateShader(Shader::Stage stage, uint32_t index, const std::wstring &fileName);
		Blob		GetShader(Shader::Stage stage, uint32_t index) const;
		Shader::Reflector GetReflector(Shader::Stage stage, uint32_t index) const;

	protected:
		Blob		&checkShaderStorage(Shader::Stage stage, uint32_t index);
		Shader::Reflector &checkReflectorStorage(Shader::Stage stage, uint32_t index);

		std::vector<Blob> m_shaders[Shader::NUM_STAGE];
		std::vector<Shader::Reflector> m_reflectors[Shader::NUM_STAGE];
	};
}
