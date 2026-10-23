#include "core/objectstyle.h"
#include "core/design.h"
#include "core/imageasset.h"
QStringList ObjectStyles::keys() {
  return {"fill",          "fillToken",     "fillStyle",        "fillSecondary",
          "fillAngle",     "patternStyle",  "opacity",          "cornerRadius",
          "strokeColor",   "strokeWidth",   "strokeStyle",      "strokeJoin",
          "strokeCap",     "shadowEnabled", "shadowColor",      "shadowX",
          "shadowY",       "fontSize",      "fontWeight",       "fontFamily",
          "fontToken",     "textColor",     "textColorToken",   "uppercase",
          "letterSpacing", "italic",        "underline",        "textAlign",
          "verticalAlign", "lineHeight",    "paragraphSpacing", "textIndent",
          "listStyle",     "listStart",     "textFit",
          // EFFECTS-UI-1: glow and shadow travel with a saved appearance.
          "shadowBlur",    "shadowInner",   "shadowScaleX",     "shadowScaleY",
          "shadowSkewX",   "shadowSkewY",   "shadowAlign",      "glowRadius",
          "glowColor",     "textGlowRadius", "textGlowColor",
          // BOTH-EFFECTS-1: a text box's own shadow.
          "boxShadowEnabled", "boxShadowColor", "boxShadowX", "boxShadowY", "boxShadowBlur", "boxShadowInner",
          "boxShadowScaleX", "boxShadowScaleY", "boxShadowSkewX", "boxShadowSkewY", "boxShadowAlign",
          // FILL-UI-1: the rest of PowerPoint's fill and line, and a text box's own box.
          "gradientStops", "gradientPath", "gradientFocusL", "gradientFocusT", "gradientFocusR", "gradientFocusB",
          "gradientScaled", "gradientRotate", "patternPreset", "pictureTile", "pictureTileX", "pictureTileY",
          "pictureTileScaleX", "pictureTileScaleY", "pictureTileAlign", "pictureTileFlip", "pictureInsetL", "pictureInsetT",
          "pictureInsetR", "pictureInsetB", "pictureTransparency", "pictureRotate", "strokeDash", "strokeDashCustom",
          "strokeCompound", "strokeGradient", "strokeStops", "strokeAngle", "strokeGradientPath",
          "boxDrawn", "boxInsetL", "boxInsetT", "boxInsetR", "boxInsetB"};
}
SceneObject ObjectStyles::capture(const SceneObject &object) {
  SceneObject appearance;
  const auto properties = Design::properties(object);
  for (const auto &key : keys())
    Design::setProperty(appearance, key, properties.value(key), false);
  if (object.fillStyle == 4) {
    ImageAsset::copyData(appearance, object);
    appearance.imageMode = object.imageMode;
    appearance.imageCrop = object.imageCrop;
  }
  return appearance;
}
void ObjectStyles::apply(SceneObject &object, const SceneObject &appearance) {
  const auto properties = Design::properties(appearance);
  for (const auto &key : keys()) {
    Design::setProperty(object, key, properties.value(key), false);
    Design::markOverride(object, key);
  }
  if (appearance.fillStyle == 4 && (object.type == ObjectType::Rect || object.type == ObjectType::Text)) {
    ImageAsset::copyData(object, appearance);
    object.imageMode = appearance.imageMode;
    object.imageCrop = appearance.imageCrop;
    for (const auto &key :
         {"imageId", "imageMode", "cropX", "cropY", "cropW", "cropH"})
      Design::markOverride(object, key);
  }
}
