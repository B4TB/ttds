{-# LANGUAGE ForeignFunctionInterface #-}
{-# LANGUAGE LambdaCase #-}
{-# LANGUAGE OverloadedStrings #-}

module Main where

import Auth (TokenStore, checkAuth, initTokenStore, register, unregister, verifyAdmin, AuthStatus(..))
import Control.Monad.IO.Class (liftIO)
import qualified Data.Text as T
import Data.Text.Lazy (pack, toStrict)
import Data.UUID (toText)
import GHC.Conc (atomically)
import Network.HTTP.Types.Status (badRequest400, unauthorized401, forbidden403, conflict409, internalServerError500)
import Proc (Proc, call, kill, launch, mkCommand, mkUnvalidatedCommand, mkComp)
import System.Environment (getArgs)
import System.Posix.Signals (Handler (..), installHandler, sigINT)
import Web.Scotty (ScottyM, ActionM, capture, delete, finish, header, notFound, pathParam, post, queryParam, scotty, status, text)

foreign import ccall "reallyExit" reallyExit :: IO ()

main :: IO ()
main = getArgs >>= launch >>= setupAndRun
  where
    setupAndRun proc = setupTerm proc >> runWithProc proc
    runWithProc proc = initTokenStore >>= runWebServer proc

    setupTerm proc = installHandler sigINT (sigintHandler proc) Nothing
    sigintHandler proc =
      Catch $
        putStrLn "Exiting." >> kill proc >> reallyExit

runWebServer :: Proc -> TokenStore -> IO ()
runWebServer proc ts =
  scotty 8080 $ do
    post "/raw/:text" $ requireAdmin ts >> pathParam "text" >>= routeRaw
    post "/pane/:pane/create" $ do
      pane <- pathParam "pane"
      color <- queryParam "color"
      liftIO $ putStrLn color
      callCreate pane color
      registerPane (T.pack pane)

    makeDrawRoute "RECT" "rect" ["color", "x", "y", "w", "h"]
    makeDrawRoute "CIRCLE" "circle" ["color", "x", "y", "r"]
    makeDrawRoute "LINE" "line" ["color", "x", "y", "x2", "y2"]
    makeDrawRoute "COPY_RECT" "copy_rect" ["x", "y", "x2", "y2", "w", "h"]
    makeDrawRoute "BEZIER2" "bezier2" ["color", "x0", "y0", "x1", "y1", "x2", "y2"]

    delete "/pane/:pane" $
      pathParam "pane" >>= checkAuthScotty >>= \pane ->
        callDelete (T.unpack pane) >> liftIO (unregister ts pane)

    notFound $ text "404\n"
  where
    makeDrawRoute :: String -> String -> [String] -> ScottyM ()
    makeDrawRoute cmd slug args = post (capture $ "/pane/:pane/" ++ slug) $ do
      pane <- pathParam "pane"
      vals <- mapM (queryParam . pack) args
      _ <- checkAuthScotty pane
      callCmd $ mkCommand ((mkComp . T.unpack) pane) (mkComp cmd) (map mkComp vals)

    callAct cmd = liftIO $ call proc cmd
    routeRaw cmd = (callAct . mkUnvalidatedCommand) cmd >>= text . pack

    registerPane name =
      (liftIO . register ts) name >>= \case
        Just uuid -> (text . pack . T.unpack . toText) uuid
        -- If we're here, this means we've already created the pane in the C
        -- layer. If the pane already exists in our memory, then, we've done
        -- something wrong.
        Nothing -> status internalServerError500 >> text "Pane with same name exists." >> finish

    checkAuthScotty name = header "Auth" >>= check >>= serve
      where
        check (Just uuid) = liftIO $ checkAuth ts name uuid
        check Nothing = status unauthorized401 >> finish

        serve Allowed = return name
        serve Disallowed = status forbidden403 >> finish
	serve BadToken = status unauthorized401 >> finish

    callCreate name color = callCmd $ mkCommand (mkComp name) (mkComp "CREATE") [mkComp color]

    callDelete name = callCmd $ mkCommand (mkComp name) (mkComp "REMOVE") []

    callCmd cmd = callAct cmd >>= \case
      "OK" -> return ()
      x -> setStatusFromErr x >> (text . pack) x >> finish

    setStatusFromErr "act_create: failed: duplicate pane" = status conflict409
    setStatusFromErr x = status internalServerError500

requireAdmin :: TokenStore -> ActionM ()
requireAdmin ts = header "Auth" >>= isOk >>= verify
  where
    isOk (Just token) = liftIO $ atomically $ verifyAdmin ts $ toStrict token
    isOk Nothing = return False

    verify True = return ()
    verify False = status unauthorized401 >> finish
