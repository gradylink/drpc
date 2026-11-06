function(_recipe_drpc_source)
  set(DRPC_REF "main")
	if(CL_VERSION_REQ)
    set(DRPC_REF "${CL_REQ_VERSION}")
	endif()

	cl_import_source(
		NAME drpc
		REPO https://github.com/gradylink/drpc.git
    REF ${DRPC_REF}
    OPTIONS "BUILD_EXAMPLES OFF"
	)
endfunction()
