#pragma once

#include "../batcher.hpp"
#include "../uniform_values_storage.hpp"

class ListGenerator
{
public:
  ListGenerator();

  void SetDepth(float depth);
  void SetViewport(float x, float y, float width, float height);
  void SetProgram(uint32_t program);
  void SetUniforms(const UniformValuesStorage & uniforms);

  void Generate(int count, Batcher & batcher);

private:
  float m_depth;
  float m_x, m_y, m_width, m_height;
  uint32_t m_programIndex;
  UniformValuesStorage m_uniforms;
};
