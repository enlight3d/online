/* eslint-disable */
/* See CanvasSectionContainer.ts for explanations. */

declare var L: any;
declare var $: any;
declare var Hammer: any;
declare var app: any;

class TilesSection {
	context: CanvasRenderingContext2D = null;
	myTopLeft: Array<number> = null;
	documentTopLeft: Array<number> = null;
	containerObject: any = null;
	dpiScale: number = null;
	name: string = null;
	backgroundColor: string = null;
	borderColor: string = null;
	boundToSection: string = null;
	anchor: Array<any> = new Array(0);
	position: Array<number> = new Array(0);
	size: Array<number> = new Array(0);
	expand: Array<string> = new Array(0);
	isLocated: boolean = false;
	processingOrder: number = null;
	drawingOrder: number = null;
	zIndex: number = null;
	interactable: boolean = true;
	sectionProperties: any = {};
	map: any;
	offscreenCanvases: Array<any> = new Array(0);
	oscCtxs: Array<any> = new Array(0);

	constructor () {
		this.name = L.CSections.Tiles.name;
		// Below anchor list may be expanded. For example, Writer may have ruler section. Then ruler section should also be added here.
		this.anchor = [[L.CSections.ColumnHeader.name, 'bottom', 'top'], [L.CSections.RowHeader.name, 'right', 'left']];
		this.position = [0, 0]; // This section's myTopLeft will be anchored to other sections^. No initial position is needed.
		this.size = [0, 0]; // Going to be expanded, no initial width or height is necessary.
		this.expand = ['top', 'left', 'bottom', 'right'];
		this.processingOrder = L.CSections.Tiles.processingOrder;
		this.drawingOrder = L.CSections.Tiles.drawingOrder;
		this.zIndex = L.CSections.Tiles.zIndex;

		this.map = L.Map.THIS;

		this.sectionProperties.docLayer = this.map._docLayer;
		this.sectionProperties.tsManager = this.sectionProperties.docLayer._painter;
	}

	public onInitialize () {
		for (var i = 0; i < 4; i++) {
			this.offscreenCanvases.push(document.createElement('canvas'));
			this.oscCtxs.push(this.offscreenCanvases[i].getContext('2d', { alpha: false }));
		}
		this.onResize();
	}

	public onResize () {
		var tileSize = this.sectionProperties.docLayer._getTileSize();
		var borderSize = 3;
		this.sectionProperties.osCanvasExtraSize = 2 * borderSize * tileSize;
		for (var i = 0; i < 4; ++i) {
			this.offscreenCanvases[i].width = this.size[0] + this.sectionProperties.osCanvasExtraSize;
			this.offscreenCanvases[i].height = this.size[1] + this.sectionProperties.osCanvasExtraSize;
		}
	}

	extendedPaneBounds (paneBounds: any) {
		var extendedBounds = paneBounds.clone();
		var halfExtraSize = this.sectionProperties.osCanvasExtraSize / 2; // This is always an integer.
		var spCxt = this.sectionProperties.docLayer.getSplitPanesContext();
		if (spCxt) {
			var splitPos = spCxt.getSplitPos().multiplyBy(this.dpiScale);
			if (paneBounds.min.x) { // pane can move in x direction.
				extendedBounds.min.x = Math.max(splitPos.x, extendedBounds.min.x - halfExtraSize);
				extendedBounds.max.x += halfExtraSize;
			}
			if (paneBounds.min.y) { // pane can move in y direction.
				extendedBounds.min.y = Math.max(splitPos.y, extendedBounds.min.y - halfExtraSize);
				extendedBounds.max.y += halfExtraSize;
			}
		}
		else {
			extendedBounds.min.x -= halfExtraSize;
			extendedBounds.max.x += halfExtraSize;
			extendedBounds.min.y -= halfExtraSize;
			extendedBounds.max.y += halfExtraSize;
		}

		return extendedBounds;
	}

	paintWithPanes (tile: any, ctx: any, async: boolean) {
		var tileTopLeft = tile.coords.getPos();
		var tileBounds = new L.Bounds(tileTopLeft, tileTopLeft.add(ctx.tileSize));

		for (var i = 0; i < ctx.paneBoundsList.length; ++i) {
			// co-ordinates of this pane in core document pixels
			var paneBounds = ctx.paneBoundsList[i];
			// co-ordinates of the main-(bottom right) pane in core document pixels
			var viewBounds = ctx.viewBounds;
			// Extended pane bounds
			var extendedBounds = this.extendedPaneBounds(paneBounds);

			// into real pixel-land ...
			paneBounds.round();
			viewBounds.round();
			extendedBounds.round();

			if (paneBounds.intersects(tileBounds)) {
				var paneOffset = paneBounds.getTopLeft(); // allocates
				// Cute way to detect the in-canvas pixel offset of each pane
				paneOffset.x = Math.min(paneOffset.x, viewBounds.min.x);
				paneOffset.y = Math.min(paneOffset.y, viewBounds.min.y);

				this.drawTileInPane(tile, tileBounds, paneBounds, paneOffset, this.context, async);
			}

			if (extendedBounds.intersects(tileBounds)) {
				var offset = extendedBounds.getTopLeft();
				this.drawTileInPane(tile, tileBounds, extendedBounds, offset, this.oscCtxs[i], async);
			}
		}
	}

	drawTileInPane (tile: any, tileBounds: any, paneBounds: any, paneOffset: any, canvasCtx: any, clearBackground: boolean) {
		// intersect - to avoid state thrash through clipping
		var crop = new L.Bounds(tileBounds.min, tileBounds.max);
		crop.min.x = Math.max(paneBounds.min.x, tileBounds.min.x);
		crop.min.y = Math.max(paneBounds.min.y, tileBounds.min.y);
		crop.max.x = Math.min(paneBounds.max.x, tileBounds.max.x);
		crop.max.y = Math.min(paneBounds.max.y, tileBounds.max.y);

		var cropWidth = crop.max.x - crop.min.x;
		var cropHeight = crop.max.y - crop.min.y;

		if (cropWidth && cropHeight) {
			if (clearBackground || this.containerObject.isZoomChanged() || canvasCtx !== this.context) {
				// Whole canvas is not cleared after zoom has changed, so clear it per tile as they arrive.
				canvasCtx.fillStyle = this.containerObject.getClearColor();
				canvasCtx.fillRect(
					crop.min.x - paneOffset.x,
					crop.min.y - paneOffset.y,
					cropWidth, cropHeight);
				var gridSection = this.containerObject.getSectionWithName(L.CSections.CalcGrid.name);
				gridSection.onDrawArea(crop, paneOffset, canvasCtx);
			}
			canvasCtx.drawImage(tile.el,
				crop.min.x - tileBounds.min.x,
				crop.min.y - tileBounds.min.y,
				cropWidth, cropHeight,
				crop.min.x - paneOffset.x,
				crop.min.y - paneOffset.y,
				cropWidth, cropHeight);
		}

		if (this.sectionProperties.docLayer._debug)
		{
			canvasCtx.strokeStyle = 'rgba(255, 0, 0, 0.5)';
			canvasCtx.strokeRect(tile.coords.x - paneBounds.min.x, tile.coords.y - paneBounds.min.y, 256, 256);
		}
	}

	pdfViewDrawTileBorders (tile: any, offset: any, tileSize: number) {
		this.context.strokeStyle = 'red';
		this.context.strokeRect(offset.x, offset.y, tileSize, tileSize);
		this.context.font = '20px Verdana';
		this.context.fillStyle = 'black';
		this.context.fillText(tile.coords.x + ' ' + tile.coords.y + ' ' + tile.coords.part + ' ' + (tile.loaded ? 'y': 'n'), Math.round(offset.x + tileSize * 0.5), Math.round(offset.y + tileSize * 0.5));
	}

	paintSimple (tile: any, ctx: any, async: boolean) {
		ctx.viewBounds.round();
		var offset = new L.Point(tile.coords.getPos().x - ctx.viewBounds.min.x, tile.coords.getPos().y - ctx.viewBounds.min.y);
		var halfExtraSize = this.sectionProperties.osCanvasExtraSize / 2;
		var extendedOffset = offset.add(new L.Point(halfExtraSize, halfExtraSize));

		if ((async || this.containerObject.isZoomChanged()) && !app.file.fileBasedView) {
			// Non Calc tiles(handled by paintSimple) can have transparent pixels,
			// so clear before paint if the call is an async one.
			// For the full view area repaint, whole canvas is cleared by section container.
			// Whole canvas is not cleared after zoom has changed, so clear it per tile as they arrive even if not async.
			this.context.fillStyle = this.containerObject.getClearColor();
			this.context.fillRect(offset.x, offset.y, ctx.tileSize.x, ctx.tileSize.y);
		}

		if (app.file.fileBasedView) {
			var tileSize = this.sectionProperties.docLayer._tileSize;
			var ratio = tileSize / this.sectionProperties.docLayer._tileHeightTwips;
			var partHeightPixels = Math.round((this.sectionProperties.docLayer._partHeightTwips + this.sectionProperties.docLayer._spaceBetweenParts) * ratio);

			offset.y = tile.coords.part * partHeightPixels + tile.coords.y - this.documentTopLeft[1];
			extendedOffset.y = offset.y + halfExtraSize;

			this.context.drawImage(tile.el, offset.x, offset.y, tileSize, tileSize);
			this.oscCtxs[0].drawImage(tile.el, extendedOffset.x, extendedOffset.y, tileSize, tileSize);
			//this.pdfViewDrawTileBorders(tile, offset, tileSize);
		}
		else {
			this.context.drawImage(tile.el, offset.x, offset.y, ctx.tileSize.x, ctx.tileSize.y);
			this.oscCtxs[0].drawImage(tile.el, extendedOffset.x, extendedOffset.y, ctx.tileSize.x, ctx.tileSize.y);
		}
	}

	public paint (tile: any, ctx: any, async: boolean = false) {
		if (this.containerObject.isInZoomAnimation() || this.sectionProperties.tsManager.waitForTiles())
			return;

		if (!ctx)
			ctx = this.sectionProperties.tsManager._paintContext();

		this.containerObject.setPenPosition(this);

		if (ctx.paneBoundsActive === true)
			this.paintWithPanes(tile, ctx, async);
		else
			this.paintSimple(tile, ctx, async);
	}

	private forEachTileInView(zoom: number, part: number, ctx: any,
		callback: (tile: any, coords: any) => boolean) {
		var docLayer = this.sectionProperties.docLayer;
		var tileRanges = ctx.paneBoundsList.map(docLayer._pxBoundsToTileRange, docLayer);

		if (app.file.fileBasedView) {
			var coordList: Array<any> = this.sectionProperties.docLayer._updateFileBasedView(true);

			for (var k: number = 0; k < coordList.length; k++) {
				var key = coordList[k].key();
				var tile = docLayer._tiles[key];
				if (!callback(tile, coordList[k]))
					return;
			}
		}
		else {
			for (var rangeIdx = 0; rangeIdx < tileRanges.length; ++rangeIdx) {
				var tileRange = tileRanges[rangeIdx];
				for (var j = tileRange.min.y; j <= tileRange.max.y; ++j) {
					for (var i: number = tileRange.min.x; i <= tileRange.max.x; ++i) {
						var coords = new L.TileCoordData(
							i * ctx.tileSize.x,
							j * ctx.tileSize.y,
							zoom,
							part);

						var key = coords.key();
						var tile = docLayer._tiles[key];

						if (!callback(tile, coords))
							return;
					}
				}
			}
		}
	}

	public haveAllTilesInView(zoom?: number, part?: number, ctx?: any): boolean {
		zoom = zoom || Math.round(this.map.getZoom());
		part = part || this.sectionProperties.docLayer._selectedPart;
		ctx = ctx || this.sectionProperties.tsManager._paintContext();

		var allTilesLoaded = true;
		this.forEachTileInView(zoom, part, ctx, function (tile: any): boolean {
			// Ensure tile is loaded.
			if (!tile || !tile.loaded) {
				allTilesLoaded = false;
				return false; // stop search.
			}
			return true; // continue checking remaining tiles.
		});

		return allTilesLoaded;
	}

	public onDraw () {
		if (this.containerObject.isInZoomAnimation())
			return;

		var zoom = Math.round(this.map.getZoom());
		var part = this.sectionProperties.docLayer._selectedPart;

		// Calculate all this here intead of doing it per tile.
		var ctx = this.sectionProperties.tsManager._paintContext();

		if (this.sectionProperties.tsManager.waitForTiles()) {
			if (!this.haveAllTilesInView(zoom, part, ctx))
				return;
		}

		for (var i = 0; i < ctx.paneBoundsList.length; ++i) {
			this.oscCtxs[i].fillStyle = this.containerObject.getClearColor();
			this.oscCtxs[i].fillRect(0, 0, this.offscreenCanvases[i].width, this.offscreenCanvases[i].height);
		}

		var docLayer = this.sectionProperties.docLayer;
		var tileSection = this;
		var doneTiles = new Set();
		this.forEachTileInView(zoom, part, ctx, function (tile: any, coords: any): boolean {
			if (doneTiles.has(coords.key()))
				return true;

			// Ensure tile is loaded and is within document bounds.
			if (tile && tile.loaded && docLayer._isValidTile(coords)) {
				tileSection.paint(tile, ctx, false /* async? */);
			}
			doneTiles.add(coords.key());
			return true; // continue with remaining tiles.
		});
	}

	private forEachTileInArea(area: any, zoom: number, part: number, ctx: any,
		callback: (tile: any, coords: any) => boolean) {
		var docLayer = this.sectionProperties.docLayer;
		var tileRange = docLayer._pxBoundsToTileRange(area);

		for (var j = tileRange.min.y; j <= tileRange.max.y; ++j) {
			for (var i = tileRange.min.x; i <= tileRange.max.x; ++i) {
				var coords = new L.TileCoordData(
					i * ctx.tileSize.x,
					j * ctx.tileSize.y,
					zoom,
					part);

				var key = coords.key();
				var tile = docLayer._tiles[key];
				callback(tile, coords);
			}
		}
	}

	// Called by tsManager to draw a zoom animation frame.
	public drawZoomFrame(ctx?: any) {
		var tsManager = this.sectionProperties.tsManager;
		if (!tsManager._inZoomAnim)
			return;

		var scale = tsManager._zoomFrameScale;
		if (!scale || !tsManager._newCenter)
			return;

		ctx = ctx || this.sectionProperties.tsManager._paintContext();
		var docLayer = this.sectionProperties.docLayer;
		var zoom = Math.round(this.map.getZoom());
		var part = docLayer._selectedPart;
		var splitPos = ctx.splitPos;

		this.containerObject.setPenPosition(this);
		var viewSize = ctx.viewBounds.getSize();
		// clear the document area first.
		this.context.fillStyle = this.containerObject.getClearColor();
		this.context.fillRect(0, 0, viewSize.x, viewSize.y);

		var paneBoundsList = ctx.paneBoundsList;

		for (var k = 0; k < paneBoundsList.length ; ++k) {
			var paneBounds = paneBoundsList[k];
			var paneSize = paneBounds.getSize();

			// Calculate top-left in doc core-pixels for the frame.
			var docPos = tsManager._getZoomDocPos(tsManager._newCenter, paneBounds, splitPos, scale, false /* findFreePaneCenter? */);

			var destPos = new L.Point(0, 0);
			var docAreaSize = paneSize.divideBy(scale);
			if (paneBoundsList.length > 1) {
				if (paneBounds.min.x) {
					// Pane is free to move in X direction.
					destPos.x = splitPos.x;
					paneSize.x -= splitPos.x;
				} else {
					// Pane is fixed in X direction.
					docAreaSize.x = paneSize.x;
				}

				if (paneBounds.min.y) {
					// Pane is free to move in Y direction.
					destPos.y = splitPos.y;
					paneSize.y -= splitPos.y;
				} else {
					// Pane is fixed in Y direction.
					docAreaSize.y = paneSize.y;
				}
			}

			var docRange = new L.Bounds(docPos.topLeft, docPos.topLeft.add(docAreaSize));
			if (tsManager._calcGridSection) {
				tsManager._calcGridSection.onDrawArea(docRange, docRange.min.subtract(destPos), this.context);
			}
			var canvasContext = this.context;

			this.forEachTileInArea(docRange, zoom, part, ctx, function (tile: any, coords: any): boolean {
				if (!tile || !tile.loaded || !docLayer._isValidTile(coords))
					return false;

				var tileBounds = new L.Bounds(tile.coords.getPos(), tile.coords.getPos().add(ctx.tileSize));
				var crop = new L.Bounds(tileBounds.min, tileBounds.max);
				crop.min.x = Math.max(docRange.min.x, tileBounds.min.x);
				crop.min.y = Math.max(docRange.min.y, tileBounds.min.y);
				crop.max.x = Math.min(docRange.max.x, tileBounds.max.x);
				crop.max.y = Math.min(docRange.max.y, tileBounds.max.y);

				var cropWidth = crop.max.x - crop.min.x;
				var cropHeight = crop.max.y - crop.min.y;

				var tileOffset = crop.min.subtract(tileBounds.min);
				var paneOffset = crop.min.subtract(docRange.min.subtract(destPos));
				if (cropWidth && cropHeight) {
					canvasContext.drawImage(tile.el,
						tileOffset.x, tileOffset.y, // source x, y
						cropWidth, cropHeight, // source size
						// Destination x, y, w, h (In non-Chrome browsers it leaves lines without the 0.5 correction).
						Math.floor(paneOffset.x * scale) + 0.5, // Destination x
						Math.floor(paneOffset.y * scale) + 0.5, // Destination y
						Math.floor(cropWidth * scale) + 1.5,    // Destination width
						Math.floor(cropHeight * scale) + 1.5);  // Destination height
				}

				return true;
			}); // end of forEachTileInArea call.

		} // End of pane bounds list loop.

	}

	public onMouseWheel () {}
	public onMouseMove () {}
	public onMouseDown () {}
	public onMouseUp () {}
	public onMouseEnter () {}
	public onMouseLeave () {}
	public onClick () {}
	public onDoubleClick () {}
	public onContextMenu () {}
	public onLongPress () {}
	public onMultiTouchStart () {}
	public onMultiTouchMove () {}
	public onMultiTouchEnd () {}
	public onNewDocumentTopLeft () {}
}

L.getNewTilesSection = function () {
	return new TilesSection();
}
