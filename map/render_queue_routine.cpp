#include "../base/SRC_FIRST.hpp"
#include "../base/mutex.hpp"
#include "../base/ptr_utils.hpp"
#include "../base/timer.hpp"
#include "../base/logging.hpp"

#include "../std/bind.hpp"

#include "../yg/internal/opengl.hpp"
#include "../yg/render_state.hpp"
#include "../yg/rendercontext.hpp"
#include "../yg/framebuffer.hpp"
#include "../yg/renderbuffer.hpp"
#include "../yg/texture.hpp"
#include "../yg/resource_manager.hpp"
#include "../yg/screen.hpp"
#include "../yg/pen_info.hpp"
#include "../yg/skin.hpp"

#include "../indexer/scales.hpp"

#include "events.hpp"
#include "drawer_yg.hpp"
#include "window_handle.hpp"
#include "render_queue_routine.hpp"
RenderQueueRoutine::RenderModelCommand::RenderModelCommand(ScreenBase const & frameScreen,
                                                             render_fn_t renderFn)
                                                               : m_frameScreen(frameScreen),
                                                               m_renderFn(renderFn)
{}

RenderQueueRoutine::RenderQueueRoutine(shared_ptr<yg::gl::RenderState> const & renderState,
                                       string const & skinName,
                                       bool isMultiSampled)
{
  m_skinName = skinName;
  m_visualScale = 0;
  m_renderState = renderState;
  m_renderState->addInvalidateFn(bind(&RenderQueueRoutine::invalidate, this));
  m_isMultiSampled = isMultiSampled;
}

void RenderQueueRoutine::Cancel()
{
  IRoutine::Cancel();
  /// Waking up the sleeping thread...
  m_hasRenderCommands.Signal();
  /// ...Or cancelling the current rendering command in progress.
  if (m_currentRenderCommand != 0)
    m_currentRenderCommand->m_paintEvent->setIsCancelled(true);
}

void RenderQueueRoutine::processResize(ScreenBase const & /*frameScreen*/)
{
  threads::MutexGuard guard(*m_renderState->m_mutex.get());

  if (m_renderState->m_isResized)
  {
    size_t texW = m_renderState->m_textureWidth;
    size_t texH = m_renderState->m_textureHeight;

    m_renderState->m_backBuffer.reset();
    m_renderState->m_backBuffer = make_shared_ptr(new yg::gl::RawRGBA8Texture(texW, texH));

    m_renderState->m_depthBuffer.reset();
    m_renderState->m_depthBuffer = make_shared_ptr(new yg::gl::RenderBuffer(texW, texH, true));

    if (!m_isMultiSampled)
      m_threadDrawer->screen()->setRenderTarget(m_renderState->m_backBuffer);

    m_threadDrawer->screen()->frameBuffer()->setDepthBuffer(m_renderState->m_depthBuffer);

    m_threadDrawer->onSize(texW, texH);

    m_renderState->m_actualTarget.reset();
    m_renderState->m_actualTarget = make_shared_ptr(new yg::gl::RawRGBA8Texture(texW, texH));

    m_threadDrawer->screen()->setRenderTarget(m_renderState->m_actualTarget);
    m_threadDrawer->screen()->beginFrame();
    m_threadDrawer->screen()->clear();
    m_threadDrawer->screen()->endFrame();

    m_threadDrawer->screen()->setRenderTarget(m_renderState->m_backBuffer);
    m_threadDrawer->screen()->beginFrame();
    m_threadDrawer->screen()->clear();
    m_threadDrawer->screen()->endFrame();

    m_renderState->m_doRepaintAll = true;

    m_renderState->m_isResized = false;
  }
}

void RenderQueueRoutine::getUpdateAreas(vector<m2::RectI> & areas)
{
  threads::MutexGuard guard(*m_renderState->m_mutex.get());
  size_t w = m_renderState->m_textureWidth;
  size_t h = m_renderState->m_textureHeight;

  if (m_renderState->m_doRepaintAll)
  {
    m_renderState->m_doRepaintAll = false;
    areas.push_back(m2::RectI(0, 0, w, h));
    return;
  }
/*
  if (m_renderState->isPanning())
  {
    /// adding two rendering sub-commands to render new areas, opened by panning.

    double dx = 0;
    double dy = 0;

    m_renderState->m_actualScreen.PtoG(dx, dy);
    m_renderState->m_currentScreen.GtoP(dx, dy);

    if (dx > 0)
      dx = ceil(dx);
    else
      dx = floor(dx);
    if (dy > 0)
      dy = ceil(dy);
    else
      dy = floor(dy);

    double minX, maxX;
    double minY, maxY;

    if (dx > 0)
      minX = 0;
    else
      minX = w + dx;

    maxX = minX + my::Abs(dx);

    if (dy > 0)
      minY = 0;
    else
      minY = h + dy;

    maxY = minY + my::Abs(dy);

    if (minX != maxX)
      areas.push_back(m2::RectI(minX, 0, maxX, h));
    if (minY != maxY)
    {
      if (minX == 0)
        areas.push_back(m2::RectI(maxX, minY, w, maxY));
      else
        areas.push_back(m2::RectI(0, minY, minX, maxY));
    }
  }
  else */
  {
    areas.push_back(m2::RectI(0, 0, w, h));

/*    int rectW = (w + 9) / 5;
    int rectH = (h + 9) / 5;
    m2::RectI r( 2 * rectW, 2 * rectH, 3 * rectW, 3 * rectH);
    areas.push_back(r);
    areas.push_back(m2::Offset(r, -rectW, 0));
    areas.push_back(m2::Offset(r, -rectW, -rectH));
    areas.push_back(m2::Offset(r, 0, -rectH));
    areas.push_back(m2::Offset(r, rectW, -rectH));
    areas.push_back(m2::Offset(r, rectW, 0));
    areas.push_back(m2::Offset(r, rectW, rectH));
    areas.push_back(m2::Offset(r, 0, rectH));
    areas.push_back(m2::Offset(r, -rectW, rectH));
    areas.push_back(m2::Offset(r, -2 * rectW, rectH));
    areas.push_back(m2::Offset(r, -2 * rectW, 0));
    areas.push_back(m2::Offset(r, -2 * rectW, -rectH));
    areas.push_back(m2::Offset(r, -2 * rectW, -2*rectH));
    areas.push_back(m2::Offset(r, -rectW, -2*rectH));
    areas.push_back(m2::Offset(r, 0, -2*rectH));
    areas.push_back(m2::Offset(r, rectW, -2*rectH));
    areas.push_back(m2::Offset(r, 2 * rectW, -2*rectH));
    areas.push_back(m2::Offset(r, 2 * rectW, -rectH));
    areas.push_back(m2::Offset(r, 2 * rectW, 0));
    areas.push_back(m2::Offset(r, 2 * rectW, rectH));
    areas.push_back(m2::Offset(r, 2 * rectW, 2*rectH));
    areas.push_back(m2::Offset(r, rectW, 2*rectH));
    areas.push_back(m2::Offset(r, 0, 2*rectH));
    areas.push_back(m2::Offset(r, -rectW, 2*rectH));
    areas.push_back(m2::Offset(r, -2*rectW, 2*rectH));
*/
  }
}

void RenderQueueRoutine::setVisualScale(double visualScale)
{
  m_visualScale = visualScale;
}

void RenderQueueRoutine::Do()
{
  m_renderContext->makeCurrent();
  m_threadDrawer = make_shared_ptr(new DrawerYG(m_resourceManager, m_skinName, !m_isMultiSampled));
  CHECK(m_visualScale != 0, ("Set the VisualScale first!"));
  m_threadDrawer->SetVisualScale(m_visualScale);
  m_threadDrawer->screen()->setIsMultiSampled(m_isMultiSampled);

  m_fakeTarget = make_shared_ptr(new yg::gl::RGBA8Texture(2, 2));

  m_frameBuffer = make_shared_ptr(new yg::gl::FrameBuffer());
  m_threadDrawer->setFrameBuffer(m_frameBuffer);

  m_threadDrawer->screen()->setRenderState(m_renderState);

  yg::gl::RenderState s;

  while (!IsCancelled())
  {
    {
      threads::ConditionGuard guard(m_hasRenderCommands);

      while (m_renderCommands.empty())
      {
        guard.Wait();
        if (IsCancelled())
          break;
      }

      if (IsCancelled())
        break;

      /// Preparing render command for execution.
      m_currentRenderCommand = m_renderCommands.front();
      m_renderCommands.erase(m_renderCommands.begin());

      processResize(m_currentRenderCommand->m_frameScreen);

      m_threadDrawer->screen()->setRenderTarget(m_renderState->m_backBuffer);

      m_currentRenderCommand->m_paintEvent = make_shared_ptr(new PaintEvent(m_threadDrawer));

      /// this prevents the framework from flooding us with a bunch of RenderCommands with the same screen.
      {
        threads::MutexGuard guard(*m_renderState->m_mutex.get());
        m_renderState->m_currentScreen = m_currentRenderCommand->m_frameScreen;
        s = *m_renderState.get();
      }
    }

    my::Timer timer;

    /// At this point renderQueue->mutex is released to allow
    /// main thread to add new rendering tasks and blit already
    /// rendered model while the current command is actually rendering.

    if (m_currentRenderCommand != 0)
    {
      /// update areas in pixel coordinates.
      vector<m2::RectI> areas;

      getUpdateAreas(areas);

      m_threadDrawer->beginFrame();

      m_threadDrawer->screen()->enableClipRect(true);
      m_threadDrawer->screen()->setClipRect(m2::RectI(0, 0, s.m_textureWidth, s.m_textureHeight));
      m_threadDrawer->clear();

//      m_threadDrawer->screen()->setClipRect(m2::RectI(0, 0, s.m_surfaceWidth, s.m_surfaceHeight));

      if (s.isPanning())
        m_threadDrawer->screen()->blit(
            s.m_actualTarget,
            s.m_actualScreen,
            s.m_currentScreen);

      ScreenBase const & frameScreen = m_currentRenderCommand->m_frameScreen;
      m2::RectD glbRect;
//      frameScreen.PtoG(m2::RectD(0, 0, m_renderState->m_surfaceWidth, m_renderState->m_surfaceHeight), glbRect);
      frameScreen.PtoG(m2::RectD(0, 0, s.m_surfaceWidth, s.m_surfaceHeight), glbRect);
      int scaleLevel = scales::GetScaleLevel(glbRect);

      for (size_t i = 0; i < areas.size(); ++i)
      {
        if ((areas[i].SizeX() != 0) && (areas[i].SizeY() != 0))
        {
          frameScreen.PtoG(m2::Inflate<double>(m2::RectD(areas[i]), 30 * m_visualScale, 30 * m_visualScale), glbRect);

          m_threadDrawer->screen()->setClipRect(areas[i]);

/*          /// Debugging ClipRect rendering.
          /// @{
          m2::RectD pxRect;
          frameScreen.GtoP(frameScreen.ClipRect(), pxRect);

          m2::PointD pts[5] = {
            pxRect.LeftBottom(),
            pxRect.LeftTop(),
            pxRect.RightTop(),
            pxRect.RightBottom(),
            pxRect.LeftBottom()
          };

          m_threadDrawer->screen()->drawPath(
              pts, 5,
              m_threadDrawer->screen()->skin()->mapPenInfo(yg::PenInfo(yg::Color(0, 0, 0, 255), 6, 0, 0, 0)),
              10000);
          /// @}*/

          m_currentRenderCommand->m_renderFn(
              m_currentRenderCommand->m_paintEvent,
              m_currentRenderCommand->m_frameScreen,
              glbRect,
              scaleLevel);
        }
      }

      m_threadDrawer->endFrame();
    }

    double duration = timer.ElapsedSeconds();

//    LOG(LINFO, ("FrameDuration: ", duration));

    if (!IsCancelled())
    {
      /// We shouldn't update the actual target as it's already happened (through callback function)
      /// in the endFrame function
      /// updateActualTarget();

      m_currentRenderCommand.reset();

      {
        threads::MutexGuard guard(*m_renderState->m_mutex.get());
        m_renderState->m_duration = duration;
      }
    }

  }

  // By VNG: We can't destroy render context in drawing thread.
  // Notify render context instead.
  m_renderContext->endThreadDrawing();
}

void RenderQueueRoutine::addWindowHandle(shared_ptr<WindowHandle> window)
{
  m_windowHandles.push_back(window);
}

void RenderQueueRoutine::invalidate()
{
  for_each(m_windowHandles.begin(),
           m_windowHandles.end(),
           bind(&WindowHandle::invalidate, _1));
}

void RenderQueueRoutine::addCommand(render_fn_t const & fn, ScreenBase const & frameScreen)
{
  /// Command queue modification is syncronized by mutex
  threads::ConditionGuard guard(m_hasRenderCommands);

  bool needToSignal = m_renderCommands.empty();

  /// Clearing a list of commands.
  m_renderCommands.clear();

  /// pushing back new RenderCommand.
  m_renderCommands.push_back(make_shared_ptr(new RenderModelCommand(frameScreen, fn)));

  /// if we are not panning, we should cancel the render command in progress to start a new one
  if ((m_currentRenderCommand != 0) && (!IsPanning(m_currentRenderCommand->m_frameScreen, frameScreen)))
    m_currentRenderCommand->m_paintEvent->setIsCancelled(true);

  if (needToSignal)
    guard.Signal();
}

void RenderQueueRoutine::initializeGL(shared_ptr<yg::gl::RenderContext> const & renderContext,
                                      shared_ptr<yg::ResourceManager> const & resourceManager)
{
  m_renderContext = renderContext;
  m_resourceManager = resourceManager;
  m_threadRenderer.init(renderContext->createShared(), m_renderState);
}

