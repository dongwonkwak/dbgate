package dashboard

import (
	"embed"
	"fmt"
	"html/template"
	"io/fs"
	"time"
)

//go:embed templates/*.html templates/partials/*.html
var templateFS embed.FS

//go:embed static/*
var staticFS embed.FS

// funcMap defines custom template functions.
var funcMap = template.FuncMap{
	"mul": func(a float64, b float64) float64 {
		return a * b
	},
	"fmtFloat": func(f float64) string {
		return fmt.Sprintf("%.2f", f)
	},
	"fmtPct": func(f float64) string {
		return fmt.Sprintf("%.2f%%", f*100)
	},
	"fmtTime": func(t time.Time) string {
		if t.IsZero() {
			return "-"
		}
		return t.Format("2006-01-02 15:04:05 UTC")
	},
	"fmtInt": func(n uint64) string {
		return fmt.Sprintf("%d", n)
	},
}

// parseTemplates loads and parses all embedded HTML templates.
func parseTemplates() (*template.Template, error) {
	tmpl := template.New("").Funcs(funcMap)

	// Walk the embedded FS and parse all .html files.
	err := fs.WalkDir(templateFS, ".", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		data, readErr := fs.ReadFile(templateFS, path)
		if readErr != nil {
			return readErr
		}
		_, parseErr := tmpl.New(path).Parse(string(data))
		return parseErr
	})
	if err != nil {
		return nil, fmt.Errorf("parse templates: %w", err)
	}
	return tmpl, nil
}
