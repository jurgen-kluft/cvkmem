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
	mainpkg := denv.NewPackage("cvkmem")
	mainpkg.AddPackage(unittestpkg)
	mainpkg.AddPackage(basepkg)

	// 'cvkmem' library
	mainlib := denv.SetupDefaultCppLibProject("cvkmem", "github.com\\jurgen-kluft\\cvkmem")
	mainlib.Dependencies = append(mainlib.Dependencies, basepkg.GetMainLib())

	// unittest project
	maintest := denv.SetupDefaultCppTestProject("cvkmem_test", "github.com\\jurgen-kluft\\cvkmem")
	maintest.Dependencies = append(maintest.Dependencies, basepkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, unittestpkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, mainlib)

	mainpkg.AddMainLib(mainlib)
	mainpkg.AddUnittest(maintest)
	return mainpkg
}
