package cvkmem

import (
	cbase "github.com/jurgen-kluft/cbase/package"
	"github.com/jurgen-kluft/ccode/denv"
	cunittest "github.com/jurgen-kluft/cunittest/package"
)

// GetPackage returns the package object of 'cvkmem'
func GetPackage() *denv.Package {
	// Dependencies
	basepkg := cbase.GetPackage()
	unittestpkg := cunittest.GetPackage()

	// The main (cvkmem) package
	mainpkg := denv.NewPackage("github.com\\jurgen-kluft", "cvkmem")
	mainpkg.AddPackage(unittestpkg)
	mainpkg.AddPackage(basepkg)

	// 'cvkmem' library
	mainlib := denv.SetupCppLibProject(mainpkg, "cvkmem")
	mainlib.AddDependencies(basepkg.GetMainLib()...)

	// unittest project
	maintest := denv.SetupCppTestProject(mainpkg, "cvkmem_test")
	maintest.AddDependencies(basepkg.GetMainLib()...)
	maintest.AddDependencies(unittestpkg.GetMainLib()...)
	maintest.AddDependency(mainlib)

	mainpkg.AddMainLib(mainlib)
	mainpkg.AddUnittest(maintest)
	return mainpkg
}
