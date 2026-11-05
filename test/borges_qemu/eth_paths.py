# The OpenCores MAC sources live in framework-espidf, wherever the packages dir is.
from pathlib import Path

Import("env")

component = Path(env.subst("$PROJECT_PACKAGES_DIR")) / "framework-espidf" / "components" / "esp_eth" / "src"
env.Append(CPPPATH=[str(component / "openeth"), str(component / "phy")])
