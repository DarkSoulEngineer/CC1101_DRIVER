# Sphinx configuration for the standalone CC1101 driver documentation.
#
# Build (dependencies managed with uv, see pyproject.toml / uv.lock):
#   cd docs
#   uv sync
#   uv run sphinx-build -b html . _build/html

project = "CC1101 Driver"
author = "Milchis Catalin-Marian"
copyright = "2026, Milchis Catalin-Marian (MIT license)"
release = "1.0"

extensions = [
    "myst_parser",          # include the repository README unchanged
    "sphinx.ext.autosectionlabel",
]

# Prefix section labels with the document name so identical headings in
# different files do not collide.
autosectionlabel_prefix_document = True

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

exclude_patterns = ["_build", ".venv"]

# The README references repo-root files (LICENSE, components/, docs/) that
# MyST cannot resolve as doc references; they stay plain links in the
# included README. Suppress only that known-noisy category.
suppress_warnings = ["myst.xref_missing", "myst.iref_missing"]

# -- HTML output -------------------------------------------------------------
# Espressif's official theme (fork of sphinx-rtd-theme used by the ESP-IDF
# Programming Guide). Requires sphinx<8; pinned in pyproject.toml.
html_theme = "sphinx_idf_theme"
html_theme_options = {
    "navigation_depth": 3,
    "collapse_navigation": False,
    "style_external_links": True,
}
html_title = "CC1101 Driver documentation"
html_static_path = []

# Theme metadata. The driver is target-independent; no target selector.
languages = ["en"]
language = "en"
project_slug = "cc1101-driver"
project_homepage = "https://github.com/DarkSoulEngineer/CC1101_DRIVER"
version = "1.0"
pdf_file = ""
versions_url = ""


def setup(app):
    # sphinx_idf_theme reads these config values but does not register them
    # itself (esp-docs normally does). Register them for standalone use.
    app.add_config_value("pdf_file", "", "html")
    app.add_config_value("idf_target_title_dict", {}, "html")