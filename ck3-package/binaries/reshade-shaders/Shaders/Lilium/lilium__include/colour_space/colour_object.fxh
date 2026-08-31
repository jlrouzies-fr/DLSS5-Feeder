
#ifdef ENABLE_COLOUR_OBJECT

  HDR10_TO_LINEAR_LUT()

  namespace CO
  {
    #define CO_PRIM_BT709  0
    #define CO_PRIM_DCIP3  1
    #define CO_PRIM_BT2020 2

    #define CO_TRC_LINEAR_NORMALISED 0
    #define CO_TRC_LINEAR_80         1
    #define CO_TRC_PQ                2

    struct Colour_Object
    {
      float3 RGB;
      uint   prim;
      uint   trc;
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
      bool   is_untouched;
#endif
    };


    namespace Get_Luminance
    {
      float Linear_Normalised
      (
        Colour_Object CO
      )
      {
        float ret;

        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                ret = dot(CO.RGB, Csp::Mat::BT709_To_XYZ[1]);
              }
              break;
              case CO_PRIM_DCIP3:
              {
                ret = dot(CO.RGB, Csp::Mat::DCIP3_To_XYZ[1]);
              }
              break;
              case CO_PRIM_BT2020:
              {
                ret = dot(CO.RGB, Csp::Mat::BT2020_To_XYZ[1]);
              }
              break;
              default:
                ret = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                ret = dot(CO.RGB, Csp::Mat::scRGB_To_XYZ_normalised[1]);
              }
              break;
              case CO_PRIM_DCIP3:
              {
                ret = dot(CO.RGB, Csp::Mat::DCIP3_80_To_XYZ_normalised[1]);
              }
              break;
              case CO_PRIM_BT2020:
              {
                ret = dot(CO.RGB, Csp::Mat::BT2020_80_To_XYZ_normalised[1]);
              }
              break;
              default:
                ret = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_PQ:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT2020:
              {
                float3 linearColour;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
                if (CO.is_untouched)
                {
                  linearColour = FetchFromHdr10ToLinearLUT(CO.RGB);
                }
                else
#endif
                {
                  linearColour = Csp::Trc::PQ_To::Linear(CO.RGB);
                }

                ret = dot(linearColour, Csp::Mat::BT2020_To_XYZ[1]);
              }
              break;
              default:
                ret = 0.f;
                break;
            }
          }
          break;
          default:
            ret = 0.f;
            break;
        }

        return ret;
      }

//      float Linear_80
//      (
//        Colour_Object CO
//      )
//      {
//        float ret;
//
//        [forcecase]
//        switch(CO.trc)
//        {
//          case CO_TRC_LINEAR_NORMALISED:
//          {
//            [forcecase]
//            switch(CO.prim)
//            {
//              case CO_PRIM_BT709:
//              {
//                ret = dot(CO.RGB, Csp::Mat::BT709_To_XYZ[1]);
//              }
//              break;
//              case CO_PRIM_DCIP3:
//              {
//                ret = dot(CO.RGB, Csp::Mat::DCIP3_To_XYZ[1]);
//              }
//              break;
//              case CO_PRIM_BT2020:
//              {
//                ret = dot(CO.RGB, Csp::Mat::BT2020_To_XYZ[1]);
//              }
//              break;
//              default:
//                ret = 0.f;
//                break;
//            }
//          }
//          break;
//          case CO_TRC_LINEAR_80:
//          {
//            [forcecase]
//            switch(CO.prim)
//            {
//              case CO_PRIM_BT709:
//              {
//                ret = dot(CO.RGB, Csp::Mat::scRGB_To_XYZ_normalised[1]);
//              }
//              break;
//              case CO_PRIM_DCIP3:
//              {
//                ret = dot(CO.RGB, Csp::Mat::DCIP3_80_To_XYZ_normalised[1]);
//              }
//              break;
//              case CO_PRIM_BT2020:
//              {
//                ret = dot(CO.RGB, Csp::Mat::BT2020_80_To_XYZ_normalised[1]);
//              }
//              break;
//              default:
//                ret = 0.f;
//                break;
//            }
//          }
//          break;
//          case CO_TRC_PQ:
//          {
//            [forcecase]
//            switch(CO.prim)
//            {
//              case CO_PRIM_BT2020:
//              {
//                ret = dot(Csp::Trc::PQ_To::Linear(CO.RGB), Csp::Mat::BT2020_To_XYZ[1]);
//              }
//              break;
//              default:
//                ret = 0.f;
//                break;
//            }
//          }
//          break;
//          default:
//            ret = 0.f;
//            break;
//        }
//
//        return ret;
//      }
    }


    namespace Convert_Csp_To
    {
      Colour_Object scRGB
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB *= 125.f;
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT709(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Mat::BT2020_To::BT709(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
                break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT709(CO.RGB);
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Mat::BT2020_To::BT709(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_PQ:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT2020:
              {
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
                if (CO.is_untouched)
                {
                  CO.RGB = FetchFromHdr10ToLinearLUT(CO.RGB);
                }
                else
#endif
                {
                  CO.RGB = Csp::Trc::PQ_To::Linear(CO.RGB);
                }

                CO.RGB = Csp::Mat::BT2020_normalised_To::scRGB(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_BT709;
        CO.trc  = CO_TRC_LINEAR_80;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object HDR10
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::BT2020(CO.RGB);

                CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT2020(CO.RGB);

                CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::scRGB_To::BT2020_normalised(CO.RGB);

                CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_80_To::BT2020_normalised(CO.RGB);

                CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB /= 125.f;

                CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_PQ:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT2020:
                break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_BT2020;
        CO.trc  = CO_TRC_PQ;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object DCIP3_80
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::DCIP3(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB *= 125.f;
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Mat::BT2020_To::DCIP3(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::DCIP3(CO.RGB);
              }
              break;
              case CO_PRIM_DCIP3:
                break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Mat::BT2020_To::DCIP3(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_PQ:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT2020:
              {
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
                if (CO.is_untouched)
                {
                  CO.RGB = FetchFromHdr10ToLinearLUT(CO.RGB);
                }
                else
#endif
                {
                  CO.RGB = Csp::Trc::PQ_To::Linear(CO.RGB);
                }

                CO.RGB = Csp::Mat::BT2020_To::DCIP3(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_DCIP3;
        CO.trc  = CO_TRC_LINEAR_80;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object BT2020_80
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::BT2020(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT2020(CO.RGB);

                CO.RGB *= 125.f;
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB *= 125.f;
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::BT2020(CO.RGB);
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT2020(CO.RGB);
              }
              break;
              case CO_PRIM_BT2020:
                break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          case CO_TRC_PQ:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT2020:
              {
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
                if (CO.is_untouched)
                {
                  CO.RGB = FetchFromHdr10ToLinearLUT(CO.RGB);
                }
                else
#endif
                {
                  CO.RGB = Csp::Trc::PQ_To::Linear(CO.RGB);
                }

                CO.RGB *= 125.f;
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_DCIP3;
        CO.trc  = CO_TRC_LINEAR_80;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }
    }

    namespace Convert_Primaries_To
    {
      Colour_Object BT709
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
                break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT709(CO.RGB);
              }
              break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Mat::BT2020_To::BT709(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_BT709;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object DCIP3
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::DCIP3(CO.RGB);
              }
              break;
              case CO_PRIM_DCIP3:
                break;
              case CO_PRIM_BT2020:
              {
                CO.RGB = Csp::Mat::BT2020_To::DCIP3(CO.RGB);
              }
              break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_DCIP3;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object BT2020
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          case CO_TRC_LINEAR_80:
          {
            [forcecase]
            switch(CO.prim)
            {
              case CO_PRIM_BT709:
              {
                CO.RGB = Csp::Mat::BT709_To::BT2020(CO.RGB);
              }
              break;
              case CO_PRIM_DCIP3:
              {
                CO.RGB = Csp::Mat::DCIP3_To::BT2020(CO.RGB);
              }
              break;
              case CO_PRIM_BT2020:
                break;
              default:
                CO.RGB = 0.f;
                break;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.prim = CO_PRIM_BT2020;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }
    }


    namespace Convert_Trc_To
    {
      Colour_Object Linear_Normalised
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
            break;
          case CO_TRC_LINEAR_80:
          {
            CO.RGB /= 125.f;
          }
          break;
          case CO_TRC_PQ:
          {
            [branch]
            if (CO.prim == CO_PRIM_BT2020)
            {
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
              if (CO.is_untouched)
              {
                CO.RGB = FetchFromHdr10ToLinearLUT(CO.RGB);
              }
              else
#endif
              {
                CO.RGB = Csp::Trc::PQ_To::Linear(CO.RGB);
              }
            }
            else
            {
              CO.RGB = 0.f;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.trc = CO_TRC_LINEAR_NORMALISED;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object Linear_80
      (
        Colour_Object CO
      )
      {
        [forcecase]
        switch(CO.trc)
        {
          case CO_TRC_LINEAR_NORMALISED:
          {
            CO.RGB *= 125.f;
          }
          break;
          case CO_TRC_LINEAR_80:
            break;
          case CO_TRC_PQ:
          {
            [branch]
            if (CO.prim == CO_PRIM_BT2020)
            {
#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
              if (CO.is_untouched)
              {
                CO.RGB = FetchFromHdr10ToLinearLUT(CO.RGB);
              }
              else
#endif
              {
                CO.RGB = Csp::Trc::PQ_To::Linear(CO.RGB);
              }

              CO.RGB *= 125.f;
            }
            else
            {
              CO.RGB = 0.f;
            }
          }
          break;
          default:
            CO.RGB = 0.f;
            break;
        }

        CO.trc = CO_TRC_LINEAR_80;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }

      Colour_Object PQ
      (
        Colour_Object CO
      )
      {
        [branch]
        if (CO.prim == CO_PRIM_BT2020)
        {
          [forcecase]
          switch(CO.trc)
          {
            case CO_TRC_LINEAR_NORMALISED:
            {
              CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB);
            }
            break;
            case CO_TRC_LINEAR_80:
            {
              CO.RGB = Csp::Trc::Linear_To::PQ(CO.RGB / 125.f);
            }
            break;
            default:
              CO.RGB = 0.f;
              break;
          }
        }
        else
        {
          CO.RGB = 0.f;
        }

        CO.trc = CO_TRC_PQ;

#if (ACTUAL_COLOUR_SPACE == CSP_HDR10)
        CO.is_untouched = false;
#endif

        return CO;
      }
    }

  }

#endif //ENABLE_COLOUR_OBJECT
