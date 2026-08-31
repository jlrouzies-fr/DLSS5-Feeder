#include "lilium__include/hdr_black_floor_fix.fxh"


#if (defined(IS_ANALYSIS_CAPABLE_API)    \
  && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB  \
    || ACTUAL_COLOUR_SPACE == CSP_HDR10) \
   || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))


void GetParams
(
  out float OutRollOffStoppingPoint,
  out float OutOldBlackPoint,
  out float OutRollOffMinusOldBlackPoint,
  out float OutMinLum,
  out float OutG22EmuWhitePointNormalised,
  out float OutGAWhitePointNormalised
)
{
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
    OutG22EmuWhitePointNormalised = Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuWhitePoint  / 80.f;
    OutGAWhitePointNormalised     = Ui::HdrBlackFloorFix::GammaAdjustment::GAWhitePoint / 80.f;
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
    OutG22EmuWhitePointNormalised = Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuWhitePoint  / 10000.f;
    OutGAWhitePointNormalised     = Ui::HdrBlackFloorFix::GammaAdjustment::GAWhitePoint / 10000.f;
#else
    OutG22EmuWhitePointNormalised = 0.f;
    OutGAWhitePointNormalised     = 0.f;
#endif


  OutRollOffStoppingPoint = Ui::HdrBlackFloorFix::Lowering::RollOffStoppingPoint / 10000.f;
  OutOldBlackPoint        = Ui::HdrBlackFloorFix::Lowering::OldBlackPoint        / 10000.f;
  float newBlackPoint     = Ui::HdrBlackFloorFix::Lowering::NewBlackPoint        / 10000.f;

  BRANCH()
  if (Ui::HdrBlackFloorFix::Lowering::ProcessingMode != PRO_MODE_RGB)
  {
    OutRollOffStoppingPoint = Csp::Trc::Linear_To::PQ(OutRollOffStoppingPoint);

    OutOldBlackPoint = Csp::Trc::Linear_To::PQ(OutOldBlackPoint);

    newBlackPoint = sign(newBlackPoint)
                  * Csp::Trc::Linear_To::PQ(abs(newBlackPoint));
  }

  OutRollOffMinusOldBlackPoint = OutRollOffStoppingPoint
                               - OutOldBlackPoint;

  OutMinLum = (newBlackPoint - OutOldBlackPoint)
            / OutRollOffMinusOldBlackPoint;

  return;
}


// Vertex shader generating a triangle covering the entire screen.
// Calculate values only "once" (3 times because it's 3 vertices)
// for the pixel shader.
void VS_PrepareHdrBlackFloorFix
(
  in                  uint   VertexID   : SV_VertexID,
  out                 float4 Position   : SV_Position
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
                                                     ,
  out nointerpolation float4 FuncParms0 : FuncParms0,
  out nointerpolation float2 FuncParms1 : FuncParms1
#endif
)
{
  float2 texCoord;
  texCoord.x = (VertexID == 2) ? 2.f
                               : 0.f;
  texCoord.y = (VertexID == 1) ? 2.f
                               : 0.f;
  Position = float4(texCoord * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);

#if (__RESHADE_PERFORMANCE_MODE__ == 0)

  FuncParms0 = 0.f;
  FuncParms1 = 0.f;


// black flower lowering
#define rollOffStoppingPoint      FuncParms0.x
#define oldBlackPoint             FuncParms0.y
#define rollOffMinusOldBlackPoint FuncParms0.z
#define minLum                    FuncParms0.w

// gamma 2.2 emulation
#define g22EmuWhitePointNormalised FuncParms1.x
// gamma adjustment
#define gaWhitePointNormalised     FuncParms1.y

  GetParams(rollOffStoppingPoint,
            oldBlackPoint,
            rollOffMinusOldBlackPoint,
            minLum,
            g22EmuWhitePointNormalised,
            gaWhitePointNormalised);

#endif
}


void PS_HdrBlackFloorFix
(
  in                  float4 Position   : SV_Position,
#if (__RESHADE_PERFORMANCE_MODE__ == 0)
  in  nointerpolation float4 FuncParms0 : FuncParms0,
  in  nointerpolation float2 FuncParms1 : FuncParms1,
#endif
  out                 float4 Output     : SV_Target0
)
{
#if (__RESHADE_PERFORMANCE_MODE__ == 1)

  static float params[6];

  GetParams(params[0],
            params[1],
            params[2],
            params[3],
            params[4],
            params[5]);

  static const float rollOffStoppingPoint       = params[0];
  static const float oldBlackPoint              = params[1];
  static const float rollOffMinusOldBlackPoint  = params[2];
  static const float minLum                     = params[3];
  static const float g22EmuWhitePointNormalised = params[4];
  static const float gaWhitePointNormalised     = params[5];

#endif

  const float4 inputColour = tex2Dfetch(SamplerBackBuffer, int2(Position.xy));

  bool processingDone = false;

  CO::Colour_Object co;

  co.RGB = inputColour.rgb;

#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)

  co.trc  = CO_TRC_LINEAR_80;
  co.prim = CO_PRIM_BT709;

#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)

  co.trc         = CO_TRC_PQ;
  co.prim        = CO_PRIM_BT2020;
  co.is_untouched = true;

#else

  co.trc  = CO_TRC_LINEAR_NORMALISED;
  co.prim = CO_PRIM_BT709;

#endif

  // optimisation
  BRANCH()
  if ( Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuEnable
   &&  Ui::HdrBlackFloorFix::GammaAdjustment::GAEnable
   && !Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuOnlyLowerBlackLevels
   &&  Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuWhitePoint == Ui::HdrBlackFloorFix::GammaAdjustment::GAWhitePoint)
  {
    Gamma22EmulationAndGammaAdjustment(co,
                                       g22EmuWhitePointNormalised,
                                       processingDone);
  }
  else
  {
    BRANCH()
    if (Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuEnable)
    {
      Gamma22Emulation(co,
                       g22EmuWhitePointNormalised,
                       processingDone);
    }

    BRANCH()
    if (Ui::HdrBlackFloorFix::GammaAdjustment::GAEnable)
    {
      GammaAdjustment(co,
                      gaWhitePointNormalised,
                      processingDone);
    }
  }

  BRANCH()
  if (Ui::HdrBlackFloorFix::Lowering::LoweringEnable)
  {
    LowerBlackFloor(co,
                    rollOffStoppingPoint,
                    oldBlackPoint,
                    rollOffMinusOldBlackPoint,
                    minLum,
                    processingDone);
  }

  [branch]
  if ((Ui::HdrBlackFloorFix::Gamma22Emu::G22EmuEnable || Ui::HdrBlackFloorFix::GammaAdjustment::GAEnable)
   && processingDone)
  {
#if (ACTUAL_COLOUR_SPACE == CSP_SCRGB)
    co = CO::Convert_Csp_To::scRGB(co);
#elif (ACTUAL_COLOUR_SPACE == CSP_HDR10)
    co = CO::Convert_Csp_To::HDR10(co);
#else
    co.RGB = 0.f;
#endif
  }
  else
  [branch]
  if (!Ui::HdrBlackFloorFix::Lowering::LoweringEnable)
  {
    // needed here
    Output = 0.f;
    discard;
  }

  Output = float4(co.RGB, inputColour.a);
}


technique lilium__hdr_black_floor_fix
<
  ui_label = "Lilium's HDR black floor fix";
>
{
  pass PS_HdrBlackFloorFix
  {
    VertexShader = VS_PrepareHdrBlackFloorFix;
     PixelShader = PS_HdrBlackFloorFix;
  }
}

#else //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))

ERROR_STUFF

technique lilium__hdr_black_floor_fix
<
  ui_label = "Lilium's HDR black floor fix (ERROR)";
>
VS_ERROR

#endif //(defined(IS_ANALYSIS_CAPABLE_API) && ((ACTUAL_COLOUR_SPACE == CSP_SCRGB || ACTUAL_COLOUR_SPACE == CSP_HDR10) || defined(MANUAL_OVERRIDE_MODE_ENABLE_INTERNAL)))
