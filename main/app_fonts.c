#include "app_fonts.h"

#include "fonts/ArialRegular12.h"
#include "fonts/CasusDotView.h"
#include "fonts/DroidSansRegular9.h"
#include "fonts/VerdanaRegular32.h"
#include "fonts/IBMCGALight8x16Light8x1616.h"
#include "fonts/TerminusTTFMedium12.h"

dgx_font_t *app_font_dashboard_time(void)
{
    return CasusDotView();
}

dgx_font_t *app_font_dashboard_title(void)
{
    return ArialRegular12();
}

dgx_font_t *app_font_dashboard_subtitle(void)
{
    return TerminusTTFMedium12();
}

dgx_font_t *app_font_dashboard_text(void)
{
    return IBMCGALight8x16Light8x1616();
}

dgx_font_t *app_font_dashboard_value_digits(void)
{
    return CasusDotView();
}

dgx_font_t *app_font_dashboard_value_fallback(void)
{
    return VerdanaRegular32();
}

dgx_font_t *app_font_dashboard_value_unit(void)
{
    return ArialRegular12();
}

dgx_font_t *app_font_provisioning_title(void)
{
    return ArialRegular12();
}

dgx_font_t *app_font_provisioning_text(void)
{
    return DroidSansRegular9();
}